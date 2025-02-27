/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2014-2019 by The qTox Project Contributors
 * Copyright © 2024-2025 The TokTok team.
 */

#include "settingswidget.h"

#include "audio/audio.h"
#include "src/core/core.h"
#include "src/core/coreav.h"
#include "src/net/updatecheck.h"
#include "src/persistence/settings.h"
#include "src/video/camerasource.h"
#include "src/widget/contentlayout.h"
#include "src/widget/form/settings/aboutform.h"
#include "src/widget/form/settings/advancedform.h"
#include "src/widget/form/settings/avform.h"
#include "src/widget/form/settings/generalform.h"
#include "src/widget/form/settings/privacyform.h"
#include "src/widget/form/settings/userinterfaceform.h"
#include "src/widget/translator.h"
#include "src/widget/widget.h"

#include <QLabel>
#include <QTabWidget>
#include <QWindow>

#include <memory>

SettingsWidget::SettingsWidget(UpdateCheck& updateCheck, IAudioControl& audio, Core* core,
                               SmileyPack& smileyPack, CameraSource& cameraSource, Settings& settings,
                               Style& style, IMessageBoxManager& messageBoxManager,
                               Profile& profile, Widget* parent)
    : QWidget(parent, Qt::Window)
{
    CoreAV* coreAV = core->getAv();
    IAudioSettings* audioSettings = &settings;
    IVideoSettings* videoSettings = &settings;

    setAttribute(Qt::WA_DeleteOnClose);

    bodyLayout = std::make_unique<QVBoxLayout>();

    settingsWidgets = std::make_unique<QTabWidget>(this);
    settingsWidgets->setTabPosition(QTabWidget::North);
    bodyLayout->addWidget(settingsWidgets.get());

    std::unique_ptr<GeneralForm> gfrm(new GeneralForm(settings, style));
    connect(gfrm.get(), &GeneralForm::updateIcons, parent, &Widget::updateIcons);

    std::unique_ptr<UserInterfaceForm> uifrm(new UserInterfaceForm(smileyPack, settings, style, this));
    std::unique_ptr<PrivacyForm> pfrm(new PrivacyForm(core, settings, style, profile));
    connect(pfrm.get(), &PrivacyForm::clearAllReceipts, parent, &Widget::clearAllReceipts);

    AVForm* rawAvfrm = new AVForm(audio, coreAV, cameraSource, audioSettings, videoSettings, style);
    std::unique_ptr<AVForm> avfrm(rawAvfrm);
    std::unique_ptr<AdvancedForm> expfrm(new AdvancedForm(settings, style, messageBoxManager));
    std::unique_ptr<AboutForm> abtfrm(new AboutForm(updateCheck, core->getSelfId().toString(), style));

    connect(&updateCheck, &UpdateCheck::updateAvailable, this, &SettingsWidget::onUpdateAvailable);

    cfgForms = {{std::move(gfrm), std::move(uifrm), std::move(pfrm), std::move(avfrm),
                 std::move(expfrm), std::move(abtfrm)}};
    for (auto& cfgForm : cfgForms)
        settingsWidgets->addTab(cfgForm.get(), cfgForm->getFormIcon(), cfgForm->getFormName());

    connect(settingsWidgets.get(), &QTabWidget::currentChanged, this, &SettingsWidget::onTabChanged);

    Translator::registerHandler([this] { retranslateUi(); }, this);
}

SettingsWidget::~SettingsWidget()
{
    Translator::unregister(this);
}

void SettingsWidget::setBodyHeadStyle(QString style)
{
    settingsWidgets->setStyle(QStyleFactory::create(style));
}

void SettingsWidget::showAbout()
{
    onTabChanged(settingsWidgets->count() - 1);
}

bool SettingsWidget::isShown() const
{
    if (settingsWidgets->isVisible()) {
        settingsWidgets->window()->windowHandle()->alert(0);
        return true;
    }

    return false;
}

void SettingsWidget::show(ContentLayout* contentLayout)
{
    contentLayout->mainContent->layout()->addWidget(settingsWidgets.get());
    settingsWidgets->show();
    onTabChanged(settingsWidgets->currentIndex());
}

void SettingsWidget::onTabChanged(int index)
{
    settingsWidgets->setCurrentIndex(index);
}

void SettingsWidget::onUpdateAvailable(void)
{
    settingsWidgets->tabBar()->setProperty("update-available", true);
    settingsWidgets->tabBar()->style()->unpolish(settingsWidgets->tabBar());
    settingsWidgets->tabBar()->style()->polish(settingsWidgets->tabBar());
}

void SettingsWidget::retranslateUi()
{
    for (size_t i = 0; i < cfgForms.size(); ++i)
        settingsWidgets->setTabText(i, cfgForms[i]->getFormName());
}
