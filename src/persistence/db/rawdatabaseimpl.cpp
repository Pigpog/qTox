/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2014-2019 by The qTox Project Contributors
 * Copyright © 2024 The TokTok team.
 */

#include "rawdatabaseimpl.h"

#include <cassert>
#include <tox/toxencryptsave.h>

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QMetaObject>
#include <QMutexLocker>

/**
 * @brief Tries to open a database.
 * @param path Path to database.
 * @param password If empty, the database will be opened unencrypted.
 * Otherwise we will use toxencryptsave to derive a key and encrypt the database.
 */
RawDatabaseImpl::RawDatabaseImpl(const QString& path_, const QString& password, const QByteArray& salt)
    : workerThread{new QThread}
    , path{path_}
    , currentSalt{salt} // we need the salt later if a new password should be set
    , currentHexKey{deriveKey(password, salt)}
{
    workerThread->setObjectName("qTox Database");
    moveToThread(workerThread.get());
    workerThread->start();

    // first try with the new salt
    if (open(path, currentHexKey)) {
        return;
    }

    // avoid opening the same db twice
    close();

    // create a backup before trying to upgrade to new salt
    bool upgrade = true;
    if (!QFile::copy(path, path + ".bak")) {
        qDebug() << "Couldn't create the backup of the database, won't upgrade";
        upgrade = false;
    }

    // fall back to the old salt
    currentHexKey = deriveKey(password);
    if (open(path, currentHexKey)) {
        // upgrade only if backup successful
        if (upgrade) {
            // still using old salt, upgrade
            if (setPassword(password)) {
                qDebug() << "Successfully upgraded to dynamic salt";
            } else {
                qWarning() << "Failed to set password with new salt";
            }
        }
    } else {
        qDebug() << "Failed to open database with old salt";
    }
}

RawDatabaseImpl::~RawDatabaseImpl()
{
    close();
    workerThread->exit(0);
    while (workerThread->isRunning())
        workerThread->wait(50);
}

/**
 * @brief Tries to open the database with the given (possibly empty) key.
 * @param path Path to database.
 * @param hexKey Hex representation of the key in string.
 * @return True if success, false otherwise.
 */
bool RawDatabaseImpl::open(const QString& path_, const QString& hexKey)
{
    if (QThread::currentThread() != workerThread.get()) {
        bool ret;
        QMetaObject::invokeMethod(this, "open", Qt::BlockingQueuedConnection, Q_RETURN_ARG(bool, ret),
                                  Q_ARG(const QString&, path_), Q_ARG(const QString&, hexKey));
        return ret;
    }

    if (!QFile::exists(path_) && QFile::exists(path_ + ".tmp")) {
        qWarning() << "Restoring database from temporary export file! Did we crash while changing "
                      "the password or upgrading?";
        QFile::rename(path_ + ".tmp", path_);
    }

    if (sqlite3_open_v2(path_.toUtf8().data(), &sqlite,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr)
        != SQLITE_OK) {
        qWarning() << "Failed to open database" << path_ << "with error:" << sqlite3_errmsg(sqlite);
        return false;
    }

    if (sqlite3_create_function(sqlite, "regexp", 2, SQLITE_UTF8, nullptr,
                                &RawDatabaseImpl::regexpInsensitive, nullptr, nullptr)) {
        qWarning() << "Failed to create function regexp";
        close();
        return false;
    }

    if (sqlite3_create_function(sqlite, "regexpsensitive", 2, SQLITE_UTF8, nullptr,
                                &RawDatabaseImpl::regexpSensitive, nullptr, nullptr)) {
        qWarning() << "Failed to create function regexpsensitive";
        close();
        return false;
    }

    if (!hexKey.isEmpty()) {
        if (!openEncryptedDatabaseAtLatestSupportedVersion(hexKey)) {
            close();
            return false;
        }
    }
    return true;
}

bool RawDatabaseImpl::openEncryptedDatabaseAtLatestSupportedVersion(const QString& hexKey)
{
    // old qTox database are saved with SQLCipher 3.x defaults. For a period after 1.16.3 but
    // before 1.17.0, databases could be partially upgraded to SQLCipher 4.0 defaults, since
    // SQLCipher 3.x isn't capable of setting all the same params. If SQLCipher 4.x happened to be
    // used, they would have been fully upgraded to 4.0 default params. We need to support all three
    // of these cases, so also upgrade to the latest possible params while we're here
    if (!setKey(hexKey)) {
        return false;
    }

    auto highestSupportedVersion = highestSupportedParams();
    if (setCipherParameters(highestSupportedVersion)) {
        if (testUsable()) {
            qInfo() << "Opened database with SQLCipher" << toString(highestSupportedVersion)
                    << "parameters";
            return true;
        }
        return updateSavedCipherParameters(hexKey, highestSupportedVersion);
    }
    qCritical() << "Failed to set latest supported SQLCipher params!";
    return false;
}

bool RawDatabaseImpl::testUsable()
{
    // this will unfortunately log a warning if it fails, even though we may expect failure
    return execNow("SELECT count(*) FROM sqlite_master;");
}

/**
 * @brief Changes stored db encryption from SQLCipher 3.x defaults to 4.x defaults
 */
bool RawDatabaseImpl::updateSavedCipherParameters(const QString& hexKey, SqlCipherParams newParams)
{
    auto currentParams = readSavedCipherParams(hexKey, newParams);
    setKey(hexKey); // setKey again because a SELECT has already been run, causing crypto settings to take effect
    if (!setCipherParameters(currentParams)) {
        return false;
    }

    const auto user_version = getUserVersion();
    if (user_version < 0) {
        return false;
    }
    if (!execNow("ATTACH DATABASE '" + path + ".tmp' AS newParams KEY \"x'" + hexKey + "'\";")) {
        return false;
    }
    if (!setCipherParameters(newParams, "newParams")) {
        return false;
    }
    if (!execNow("SELECT sqlcipher_export('newParams');")) {
        return false;
    }
    if (!execNow(QString("PRAGMA newParams.user_version = %1;").arg(user_version))) {
        return false;
    }
    if (!execNow("DETACH DATABASE newParams;")) {
        return false;
    }
    if (!commitDbSwap(hexKey)) {
        return false;
    }
    qInfo() << "Upgraded database from SQLCipher" << toString(currentParams) << "params to"
            << toString(newParams) << "params complete";
    return true;
}

bool RawDatabaseImpl::setCipherParameters(SqlCipherParams params, const QString& database)
{
    QString prefix;
    if (!database.isNull()) {
        prefix = database + ".";
    }
    // from https://www.zetetic.net/blog/2018/11/30/sqlcipher-400-release/
    const QString default3_xParams{"PRAGMA database.cipher_page_size = 1024;"
                                   "PRAGMA database.kdf_iter = 64000;"
                                   "PRAGMA database.cipher_hmac_algorithm = HMAC_SHA1;"
                                   "PRAGMA database.cipher_kdf_algorithm = PBKDF2_HMAC_SHA1;"};
    // cipher_hmac_algorithm and cipher_kdf_algorithm weren't supported in sqlcipher 3.x, so our
    // upgrade to 4 only applied some of the new params if sqlcipher 3.x was used at the time
    const QString halfUpgradedTo4Params{"PRAGMA database.cipher_page_size = 4096;"
                                        "PRAGMA database.kdf_iter = 256000;"
                                        "PRAGMA database.cipher_hmac_algorithm = HMAC_SHA1;"
                                        "PRAGMA database.cipher_kdf_algorithm = PBKDF2_HMAC_SHA1;"};
    const QString default4_xParams{
        "PRAGMA database.cipher_page_size = 4096;"
        "PRAGMA database.kdf_iter = 256000;"
        "PRAGMA database.cipher_hmac_algorithm = HMAC_SHA512;"
        "PRAGMA database.cipher_kdf_algorithm = PBKDF2_HMAC_SHA512;"
        "PRAGMA database.cipher_memory_security = ON;"}; // got disabled by default in 4.5.0, so manually enable it

    QString defaultParams;
    switch (params) {
    case SqlCipherParams::p3_0: {
        defaultParams = default3_xParams;
        break;
    }
    case SqlCipherParams::halfUpgradedTo4: {
        defaultParams = halfUpgradedTo4Params;
        break;
    }
    case SqlCipherParams::p4_0: {
        defaultParams = default4_xParams;
        break;
    }
    }

    qDebug() << "Setting SQLCipher" << toString(params) << "parameters";
    return execNow(defaultParams.replace("database.", prefix));
}

RawDatabaseImpl::SqlCipherParams RawDatabaseImpl::highestSupportedParams()
{
    // Note: This is just calling into the sqlcipher library, not touching the database.
    QString cipherVersion;
    if (!execNow(RawDatabase::Query("PRAGMA cipher_version", [&](const QVector<QVariant>& row) {
            cipherVersion = row[0].toString();
        }))) {
        qCritical() << "Failed to read cipher_version";
        return SqlCipherParams::p3_0;
    }

    auto majorVersion = cipherVersion.split('.')[0].toInt();

    SqlCipherParams highestSupportedParams;
    switch (majorVersion) {
    case 3:
        highestSupportedParams = SqlCipherParams::halfUpgradedTo4;
        break;
    case 4:
        highestSupportedParams = SqlCipherParams::p4_0;
        break;
    default:
        qCritical() << "Unsupported SQLCipher version detected!";
        return SqlCipherParams::p3_0;
    }
    qDebug() << "Highest supported SQLCipher params on this system are"
             << toString(highestSupportedParams);
    return highestSupportedParams;
}

RawDatabaseImpl::SqlCipherParams RawDatabaseImpl::readSavedCipherParams(const QString& hexKey,
                                                                        SqlCipherParams newParams)
{
    for (int i = static_cast<int>(SqlCipherParams::p3_0); i < static_cast<int>(newParams); ++i) {
        if (!setKey(hexKey)) {
            break;
        }

        if (!setCipherParameters(static_cast<SqlCipherParams>(i))) {
            break;
        }

        if (testUsable()) {
            return static_cast<SqlCipherParams>(i);
        }
    }
    qCritical() << "Failed to check saved SQLCipher params";
    return SqlCipherParams::p3_0;
}

bool RawDatabaseImpl::setKey(const QString& hexKey)
{
    // setKey again to clear old bad cipher settings
    if (!execNow("PRAGMA key = \"x'" + hexKey + "'\"")) {
        qWarning() << "Failed to set encryption key";
        return false;
    }
    return true;
}

int RawDatabaseImpl::getUserVersion()
{
    int64_t user_version;
    if (!execNow(RawDatabase::Query("PRAGMA user_version", [&](const QVector<QVariant>& row) {
            user_version = row[0].toLongLong();
        }))) {
        qCritical() << "Failed to read user_version during cipher upgrade";
        return -1;
    }
    return user_version;
}

/**
 * @brief Close the database and free its associated resources.
 */
void RawDatabaseImpl::close()
{
    if (QThread::currentThread() != workerThread.get())
        return (void)QMetaObject::invokeMethod(this, "close", Qt::BlockingQueuedConnection);

    // We assume we're in the ctor or dtor, so we just need to finish processing our transactions
    process();

    if (sqlite3_close(sqlite) == SQLITE_OK)
        sqlite = nullptr;
    else
        qWarning() << "Error closing database:" << sqlite3_errmsg(sqlite);
}

/**
 * @brief Checks, that the database is open.
 * @return True if the database was opened successfully.
 */
bool RawDatabaseImpl::isOpen()
{
    // We don't need thread safety since only the ctor/dtor can write this pointer
    return sqlite != nullptr;
}

/**
 * @brief Executes a SQL transaction synchronously.
 * @param statement Statement to execute.
 * @return Whether the transaction was successful.
 */
bool RawDatabaseImpl::execNow(const QString& statement)
{
    return execNow(Query{statement});
}

/**
 * @brief Executes a SQL transaction synchronously.
 * @param statement Statement to execute.
 * @return Whether the transaction was successful.
 */
bool RawDatabaseImpl::execNow(RawDatabase::Query statement)
{
    std::vector<Query> statements;
    statements.push_back(std::move(statement));
    return execNow(std::move(statements));
}

/**
 * @brief Executes a SQL transaction synchronously.
 * @param statements List of statements to execute.
 * @return Whether the transaction was successful.
 */
bool RawDatabaseImpl::execNow(std::vector<RawDatabase::Query> statements)
{
    if (!sqlite) {
        qWarning() << "Trying to exec, but the database is not open";
        return false;
    }

    std::atomic_bool done{false};
    std::atomic_bool success{false};

    Transaction trans;
    trans.queries = std::move(statements);
    trans.done = &done;
    trans.success = &success;
    {
        QMutexLocker<QMutex> locker{&transactionsMutex};
        pendingTransactions.push(std::move(trans));
    }

    // We can't use blocking queued here, otherwise we might process future transactions
    // before returning, but we only want to wait until this transaction is done.
    QMetaObject::invokeMethod(this, "process");
    while (!done.load(std::memory_order_acquire))
        QThread::msleep(10);

    return success.load(std::memory_order_acquire);
}

/**
 * @brief Executes a SQL transaction asynchronously.
 * @param statement Statement to execute.
 */
void RawDatabaseImpl::execLater(const QString& statement)
{
    execLater(Query{statement});
}

void RawDatabaseImpl::execLater(RawDatabase::Query statement)
{
    std::vector<Query> statements;
    statements.push_back(std::move(statement));
    execLater(std::move(statements));
}

void RawDatabaseImpl::execLater(std::vector<RawDatabase::Query> statements)
{
    if (!sqlite) {
        qWarning() << "Trying to exec, but the database is not open";
        return;
    }

    Transaction trans;
    trans.queries = std::move(statements);
    {
        QMutexLocker<QMutex> locker{&transactionsMutex};
        pendingTransactions.push(std::move(trans));
    }

    QMetaObject::invokeMethod(this, "process", Qt::QueuedConnection);
}

/**
 * @brief Waits until all the pending transactions are executed.
 */
void RawDatabaseImpl::sync()
{
    QMetaObject::invokeMethod(this, "process", Qt::BlockingQueuedConnection);
}

/**
 * @brief Changes the database password, encrypting or decrypting if necessary.
 * @param password If password is empty, the database will be decrypted.
 * @return True if success, false otherwise.
 * @note Will process all transactions before changing the password.
 */
bool RawDatabaseImpl::setPassword(const QString& password)
{
    if (!sqlite) {
        qWarning() << "Trying to change the password, but the database is not open";
        return false;
    }

    if (QThread::currentThread() != workerThread.get()) {
        bool ret;
        QMetaObject::invokeMethod(this, "setPassword", Qt::BlockingQueuedConnection,
                                  Q_RETURN_ARG(bool, ret), Q_ARG(const QString&, password));
        return ret;
    }

    // If we need to decrypt or encrypt, we'll need to sync and close,
    // so we always process the pending queue before rekeying for consistency
    process();

    if (QFile::exists(path + ".tmp")) {
        qWarning() << "Found old temporary export file while rekeying, deleting it";
        QFile::remove(path + ".tmp");
    }

    if (!password.isEmpty()) {
        QString newHexKey = deriveKey(password, currentSalt);
        if (!currentHexKey.isEmpty()) {
            if (!execNow("PRAGMA rekey = \"x'" + newHexKey + "'\"")) {
                qWarning() << "Failed to change encryption key";
                close();
                return false;
            }
        } else {
            if (!encryptDatabase(newHexKey)) {
                close();
                return false;
            }
            currentHexKey = newHexKey;
        }
    } else {
        if (currentHexKey.isEmpty())
            return true;

        if (!decryptDatabase()) {
            close();
            return false;
        }
    }
    return true;
}

bool RawDatabaseImpl::encryptDatabase(const QString& newHexKey)
{
    const auto user_version = getUserVersion();
    if (user_version < 0) {
        return false;
    }
    if (!execNow("ATTACH DATABASE '" + path + ".tmp' AS encrypted KEY \"x'" + newHexKey + "'\";")) {
        qWarning() << "Failed to export encrypted database";
        return false;
    }
    if (!setCipherParameters(SqlCipherParams::p4_0, "encrypted")) {
        return false;
    }
    if (!execNow("SELECT sqlcipher_export('encrypted');")) {
        return false;
    }
    if (!execNow(QString("PRAGMA encrypted.user_version = %1;").arg(user_version))) {
        return false;
    }
    if (!execNow("DETACH DATABASE encrypted;")) {
        return false;
    }
    return commitDbSwap(newHexKey);
}

bool RawDatabaseImpl::decryptDatabase()
{
    const auto user_version = getUserVersion();
    if (user_version < 0) {
        return false;
    }
    if (!execNow("ATTACH DATABASE '" + path
                 + ".tmp' AS plaintext KEY '';"
                   "SELECT sqlcipher_export('plaintext');")) {
        qWarning() << "Failed to export decrypted database";
        return false;
    }
    if (!execNow(QString("PRAGMA plaintext.user_version = %1;").arg(user_version))) {
        return false;
    }
    if (!execNow("DETACH DATABASE plaintext;")) {
        return false;
    }
    return commitDbSwap({});
}

bool RawDatabaseImpl::commitDbSwap(const QString& hexKey)
{
    // This is racy as hell, but nobody will race with us since we hold the profile lock
    // If we crash or die here, the rename should be atomic, so we can recover no matter
    // what
    close();
    QFile::remove(path);
    QFile::rename(path + ".tmp", path);
    currentHexKey = hexKey;
    if (!open(path, currentHexKey)) {
        qCritical() << "Failed to swap db";
        return false;
    }
    return true;
}

/**
 * @brief Moves the database file on disk to match the new path.
 * @param newPath Path to move database file.
 * @return True if success, false otherwise.
 *
 * @note Will process all transactions before renaming
 */
bool RawDatabaseImpl::rename(const QString& newPath)
{
    if (!sqlite) {
        qWarning() << "Trying to change the password, but the database is not open";
        return false;
    }

    if (QThread::currentThread() != workerThread.get()) {
        bool ret;
        QMetaObject::invokeMethod(this, "rename", Qt::BlockingQueuedConnection,
                                  Q_RETURN_ARG(bool, ret), Q_ARG(const QString&, newPath));
        return ret;
    }

    process();

    if (path == newPath)
        return true;

    if (QFile::exists(newPath))
        return false;

    close();
    if (!QFile::rename(path, newPath))
        return false;
    path = newPath;
    return open(path, currentHexKey);
}

/**
 * @brief Deletes the on disk database file after closing it.
 * @note Will process all transactions before deletions.
 * @return True if success, false otherwise.
 */
bool RawDatabaseImpl::remove()
{
    if (!sqlite) {
        qWarning() << "Trying to remove the database, but it is not open";
        return false;
    }

    if (QThread::currentThread() != workerThread.get()) {
        bool ret;
        QMetaObject::invokeMethod(this, "remove", Qt::BlockingQueuedConnection,
                                  Q_RETURN_ARG(bool, ret));
        return ret;
    }

    qDebug() << "Removing database" << path;
    close();
    return QFile::remove(path);
}

/**
 * @brief Functor used to free tox_pass_key memory.
 *
 * This functor can be used as Deleter for smart pointers.
 * @note Doesn't take care of overwriting the key.
 */
struct PassKeyDeleter
{
    void operator()(Tox_Pass_Key* pass_key)
    {
        tox_pass_key_free(pass_key);
    }
};

/**
 * @brief Derives a 256bit key from the password and returns it hex-encoded
 * @param password Password to decrypt database
 * @return String representation of key
 * @deprecated deprecated on 2016-11-06, kept for compatibility, replaced by the salted version
 */
QString RawDatabaseImpl::deriveKey(const QString& password)
{
    if (password.isEmpty())
        return {};

    const QByteArray passData = password.toUtf8();

    static_assert(TOX_PASS_KEY_LENGTH >= 32, "toxcore must provide 256bit or longer keys");

    static const uint8_t expandConstant[TOX_PASS_SALT_LENGTH + 1] =
        "L'ignorance est le pire des maux";
    const std::unique_ptr<Tox_Pass_Key, PassKeyDeleter> key(
        tox_pass_key_derive_with_salt(reinterpret_cast<const uint8_t*>(passData.data()),
                                      static_cast<std::size_t>(passData.size()), expandConstant,
                                      nullptr));
    return QString::fromUtf8(QByteArray(reinterpret_cast<char*>(key.get()) + 32, 32).toHex());
}

/**
 * @brief Derives a 256bit key from the password and returns it hex-encoded
 * @param password Password to decrypt database
 * @param salt Salt to improve password strength, must be TOX_PASS_SALT_LENGTH bytes
 * @return String representation of key
 */
QString RawDatabaseImpl::deriveKey(const QString& password, const QByteArray& salt)
{
    if (password.isEmpty()) {
        return {};
    }

    if (salt.length() != TOX_PASS_SALT_LENGTH) {
        qWarning() << "Salt length doesn't match toxencryptsave expectations";
        return {};
    }

    const QByteArray passData = password.toUtf8();

    static_assert(TOX_PASS_KEY_LENGTH >= 32, "toxcore must provide 256bit or longer keys");

    const std::unique_ptr<Tox_Pass_Key, PassKeyDeleter> key(
        tox_pass_key_derive_with_salt(reinterpret_cast<const uint8_t*>(passData.data()),
                                      static_cast<std::size_t>(passData.size()),
                                      reinterpret_cast<const uint8_t*>(salt.constData()), nullptr));
    return QString::fromUtf8(QByteArray(reinterpret_cast<char*>(key.get()) + 32, 32).toHex());
}

void RawDatabaseImpl::compileAndExecute(Transaction& trans)
{
    // In case we exit early, prepare to signal errors
    if (trans.success != nullptr)
        trans.success->store(false, std::memory_order_release);

    // Add transaction commands if necessary
    if (trans.queries.size() > 1) {
        trans.queries.insert(trans.queries.begin(), {"BEGIN;"});
        trans.queries.emplace_back("COMMIT;");
    }

    // Compile queries
    for (Query& query : trans.queries) {
        assert(query.statements.isEmpty());
        // sqlite3_prepare_v2 only compiles one statement at a time in the query,
        // we need to loop over them all
        int curParam = 0;
        const char* compileTail = query.query.data();
        do {
            // Compile the next statement
            sqlite3_stmt* stmt;
            int r;
            if ((r = sqlite3_prepare_v2(sqlite, compileTail,
                                        query.query.size()
                                            - static_cast<int>(compileTail - query.query.data()),
                                        &stmt, &compileTail))
                != SQLITE_OK) {
                qWarning() << "Failed to prepare statement" << anonymizeQuery(query.query)
                           << "and returned" << r;
                qWarning("The full error is %d: %s", sqlite3_errcode(sqlite), sqlite3_errmsg(sqlite));
                return;
            }
            query.statements += stmt;

            // Now we can bind our params to this statement
            int nParams = sqlite3_bind_parameter_count(stmt);
            if (query.blobs.size() < curParam + nParams) {
                qWarning() << "Not enough parameters to bind to query " << anonymizeQuery(query.query);
                return;
            }
            for (int i = 0; i < nParams; ++i) {
                const QByteArray& blob = query.blobs[curParam + i];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wzero-as-null-pointer-constant"
                // SQLITE_STATIC uses old-style cast and 0 as null pointer but comes from
                // system headers, so can't be fixed by us.
                constexpr auto sqliteDataType = SQLITE_STATIC;
#pragma GCC diagnostic pop
                if (sqlite3_bind_blob(stmt, i + 1, blob.data(), blob.size(), sqliteDataType)
                    != SQLITE_OK) {
                    qWarning() << "Failed to bind param" << curParam + i << "to query"
                               << anonymizeQuery(query.query);
                    return;
                }
            }
            curParam += nParams;
        } while (compileTail != query.query.data() + query.query.size());

        // Execute each statement of each query of our transaction
        for (sqlite3_stmt* stmt : query.statements) {
            int column_count = sqlite3_column_count(stmt);
            int result;
            do {
                result = sqlite3_step(stmt);

                // Execute our row callback
                if (result == SQLITE_ROW && query.rowCallback) {
                    QVector<QVariant> row;
                    for (int i = 0; i < column_count; ++i)
                        row += extractData(stmt, i);

                    query.rowCallback(row);
                }
            } while (result == SQLITE_ROW);

            if (result == SQLITE_DONE)
                continue;

            QString anonQuery = anonymizeQuery(query.query);
            switch (result) {
            case SQLITE_ERROR:
                qWarning() << "Error executing query" << anonQuery;
                return;
            case SQLITE_MISUSE:
                qWarning() << "Misuse executing query" << anonQuery;
                return;
            case SQLITE_CONSTRAINT:
                qWarning() << "Constraint error executing query" << anonQuery;
                return;
            default:
                qWarning() << "Unknown error" << result << "executing query" << anonQuery;
                return;
            }
        }

        if (query.insertCallback)
            query.insertCallback(RowId{sqlite3_last_insert_rowid(sqlite)});
    }

    if (trans.success != nullptr)
        trans.success->store(true, std::memory_order_release);
}

/**
 * @brief Implements the actual processing of pending transactions.
 * Unqueues, compiles, binds and executes queries, then notifies of results
 *
 * @warning MUST only be called from the worker thread
 */
void RawDatabaseImpl::process()
{
    assert(QThread::currentThread() == workerThread.get());

    if (!sqlite)
        return;

    forever
    {
        // Fetch the next transaction
        Transaction trans;
        {
            QMutexLocker<QMutex> locker{&transactionsMutex};
            if (pendingTransactions.empty())
                return;
            trans = std::move(pendingTransactions.front());
            pendingTransactions.pop();
        }

        compileAndExecute(trans);

        // Free our statements
        for (Query& query : trans.queries) {
            for (sqlite3_stmt* stmt : query.statements)
                sqlite3_finalize(stmt);
            query.statements.clear();
        }

        // Signal transaction results
        if (trans.done != nullptr)
            trans.done->store(true, std::memory_order_release);
    }
}

/**
 * @brief Hides public keys and timestamps in query.
 * @param query Source query, which should be anonymized.
 * @return Query without timestamps and public keys.
 */
QString RawDatabaseImpl::anonymizeQuery(const QByteArray& query)
{
    QString queryString = QString::fromUtf8(query);
    queryString.replace(QRegularExpression("chat.public_key='[A-F0-9]{64}'"),
                        "char.public_key='<HERE IS PUBLIC KEY>'");
    queryString.replace(QRegularExpression("timestamp BETWEEN \\d{5,} AND \\d{5,}"),
                        "timestamp BETWEEN <START HERE> AND <END HERE>");

    return queryString;
}

/**
 * @brief Extracts a variant from one column of a result row depending on the column type.
 * @param stmt Statement to execute.
 * @param col Number of column to extract.
 * @return Extracted data.
 */
QVariant RawDatabaseImpl::extractData(sqlite3_stmt* stmt, int col)
{
    int type = sqlite3_column_type(stmt, col);
    if (type == SQLITE_INTEGER) {
        return sqlite3_column_int64(stmt, col);
    }
    if (type == SQLITE_TEXT) {
        const char* str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
        int len = sqlite3_column_bytes(stmt, col);
        return QString::fromUtf8(str, len);
    }
    if (type == SQLITE_NULL) {
        return QVariant{};
    }
    const char* data = reinterpret_cast<const char*>(sqlite3_column_blob(stmt, col));
    int len = sqlite3_column_bytes(stmt, col);
    return QByteArray(data, len);
}

/**
 * @brief Use for create function in db for search data use regular expressions without case sensitive
 * @param ctx ctx the context in which an SQL function executes
 * @param argc number of arguments
 * @param argv arguments
 */
void RawDatabaseImpl::regexpInsensitive(sqlite3_context* ctx, int argc, sqlite3_value** argv)
{
    regexp(ctx, argc, argv,
           QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
}

/**
 * @brief Use for create function in db for search data use regular expressions without case sensitive
 * @param ctx the context in which an SQL function executes
 * @param argc number of arguments
 * @param argv arguments
 */
void RawDatabaseImpl::regexpSensitive(sqlite3_context* ctx, int argc, sqlite3_value** argv)
{
    regexp(ctx, argc, argv, QRegularExpression::UseUnicodePropertiesOption);
}

void RawDatabaseImpl::regexp(sqlite3_context* ctx, int argc, sqlite3_value** argv,
                             const QRegularExpression::PatternOptions cs)
{
    std::ignore = argc;
    QRegularExpression regex;
    const QString str1 = QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_value_text(argv[0])));
    const QString str2 = QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_value_text(argv[1])));

    regex.setPattern(str1);
    regex.setPatternOptions(cs);

    const bool b = str2.contains(regex);

    if (b) {
        sqlite3_result_int(ctx, 1);
    } else {
        sqlite3_result_int(ctx, 0);
    }
}
