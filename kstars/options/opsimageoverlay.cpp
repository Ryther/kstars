/*
    SPDX-FileCopyrightText: 2023 Hy Murveit <hy@murveit.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "opsimageoverlay.h"

#include "ksfilereader.h"
#include "kstars.h"
#include "kstarsdata.h"
#include "Options.h"
#include "skymap.h"
#include "skycomponents/skymapcomposite.h"
#include "skycomponents/imageoverlaycomponent.h"

#include <KConfigDialog>

#include <QPushButton>
#include <QFileDialog>
#include <QPushButton>

OpsImageOverlay::OpsImageOverlay() : QFrame(KStars::Instance())
{
    setupUi(this);

    m_ConfigDialog = KConfigDialog::exists("settings");

    connect(m_ConfigDialog->button(QDialogButtonBox::Apply), SIGNAL(clicked()), SLOT(slotApply()));
    connect(m_ConfigDialog->button(QDialogButtonBox::Ok), SIGNAL(clicked()), SLOT(slotApply()));

    ImageOverlayComponent *overlayComponent = dynamic_cast<ImageOverlayComponent*>(
                KStarsData::Instance()->skyComposite()->imageOverlay());
    connect(solveButton, &QPushButton::clicked, overlayComponent, &ImageOverlayComponent::startSolving,
            Qt::UniqueConnection);
    connect(abortButton, &QPushButton::clicked, overlayComponent, &ImageOverlayComponent::abortSolving,
            Qt::UniqueConnection);
    //connect(reloadButton, &QPushButton::clicked, overlayComponent, &ImageOverlayComponent::reload, Qt::UniqueConnection);
    connect(showButton, &QPushButton::clicked, overlayComponent, &ImageOverlayComponent::show, Qt::UniqueConnection);

    syncOptions();
}

QTableWidget *OpsImageOverlay::table ()
{
    return imageOverlayTable;
}

QPlainTextEdit *OpsImageOverlay::statusDisplay ()
{
    return imageOverlayStatus;
}

void OpsImageOverlay::syncOptions()
{
    kcfg_ShowImageOverlays->setChecked(Options::showImageOverlays());
    kcfg_ImageOverlayMaxDimension->setValue(Options::imageOverlayMaxDimension());
    kcfg_ImageOverlayDirectory->setText(KStarsData::Instance()->skyComposite()->imageOverlay()->directory());
}

void OpsImageOverlay::slotApply()
{
    KStarsData *data = KStarsData::Instance();
    SkyMap *map      = SkyMap::Instance();

    data->setFullTimeUpdate();
    KStars::Instance()->updateTime();
    map->forceUpdate();
}
