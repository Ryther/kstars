/*
    SPDX-FileCopyrightText: 2023 Hy Murveit <hy@murveit.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "imageoverlaycomponent.h"

#include "kstars.h"
#include "Options.h"
#include "skypainter.h"
#include "skymap.h"
#include "fitsviewer/fitsdata.h"
#include "auxiliary/kspaths.h"

#include <QTableWidget>
#include <QImageReader>
#include <QCheckBox>
#include <QtConcurrent>
#include "ekos/auxiliary/solverutils.h"
#include "ekos/auxiliary/stellarsolverprofile.h"

namespace
{

enum ColumnIndex
{
    FILENAME_COL = 0,
    //ENABLED_COL,
    //NICKNAME_COL,
    STATUS_COL,
    RA_COL,
    DEC_COL,
    ARCSEC_PER_PIXEL_COL,
    ORIENTATION_COL,
    WIDTH_COL,
    HEIGHT_COL,
    EAST_TO_RIGHT_COL,
    NUM_COLUMNS
};

QStringList HeaderNames =
{
    "Filename",
    //    "", "Nickname",
    "Status", "RA", "DEC", "A-S/px", "Angle",
    "Width", "Height", "EastRight"
};

// This needs to be syncronized with enum Status
QStringList StatusNames =
{
    "Unprocessed", "Bad File", "Solve Failed", "Error", "OK"
};


// Helper to create the image overlay table.
// Start the table, displaying the heading and timing information, common to all sessions.
void setupTable(QTableWidget *table)
{
    table->clear();
    table->setRowCount(0);
    //table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setColumnCount(NUM_COLUMNS);
    //table->verticalHeader()->setDefaultSectionSize(20);
    //table->horizontalHeader()->setStretchLastSection(true);
    table->setShowGrid(false);
    table->setWordWrap(true);

    table->setHorizontalHeaderLabels(HeaderNames);
}

void setupTextRow(QTableWidget *table, int row, int column, const QString &text)
{
    table->setRowCount(row + 1);
    QTableWidgetItem *item = new QTableWidgetItem();
    item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    item->setText(text);
    table->setItem(row, column, item);
}

bool overlaySorter(const ImageOverlay &o1, const ImageOverlay &o2)
{
    return o1.m_Filename.toLower() < o2.m_Filename.toLower();
}
}  // namespace

ImageOverlayComponent::ImageOverlayComponent(SkyComposite *parent) : SkyComponent(parent)
{
    QDir dir = QDir(KSPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/imageOverlays");
    dir.mkpath(".");
    m_Directory = dir.absolutePath();
    connect(&m_TryAgainTimer, &QTimer::timeout, this, &ImageOverlayComponent::tryAgain, Qt::UniqueConnection);

    // Get the latest from the User DB
    load();

    // Load the image files in the background.
}

bool ImageOverlayComponent::selected()
{
    return Options::showImageOverlays();
}

void ImageOverlayComponent::draw(SkyPainter *skyp)
{
#if !defined(KSTARS_LITE)
    skyp->drawImageOverlay(&m_Overlays);
#else
    Q_UNUSED(skyp);
#endif
}

void ImageOverlayComponent::setWidgets(QTableWidget *table, QPlainTextEdit *statusDisplay)
{
    m_ImageOverlayTable = table;
    m_StatusDisplay = statusDisplay;
    setupTable(table);
    updateTable();
    loadAllImageFiles();
}

void ImageOverlayComponent::updateStatus(const QString &message)
{
    if (!m_StatusDisplay)
        return;
    m_LogText.insert(0, message);
    m_StatusDisplay->setPlainText(m_LogText.join("\n"));
}

// Find all the files in the directory, see if they are in the m_Overlays.
// If no, append to the end of m_Overlays, and set status as unprocessed.
void ImageOverlayComponent::updateTable()
{
    // Get the list of files from the image overlay directory.
    QDir directory(m_Directory);
    updateStatus(QString("Updating from directory: %1").arg(m_Directory));
    QStringList images = directory.entryList(QStringList() << "*", QDir::Files);
    QSet<QString> imageFiles;
    foreach(QString filename, images)
    {
        if (!FITSData::readableFilename(filename))
            continue;
        imageFiles.insert(filename);
    }

    // Sort the files alphabetically.
    QList<QString> sortedImageFiles;
    for (const auto &fn : imageFiles)
        sortedImageFiles.push_back(fn);
    std::sort(sortedImageFiles.begin(), sortedImageFiles.end(), overlaySorter);

    // Remove any database items that aren't in the directory.
    QList<ImageOverlay> tempOverlays;
    QMap<QString, int> tempMap;
    int numDeleted = 0;
    for (int i = 0; i < m_Overlays.size(); ++i)
    {
        auto &fname = m_Overlays[i].m_Filename;
        if (sortedImageFiles.indexOf(fname) >= 0)
        {
            tempOverlays.append(m_Overlays[i]);
            tempMap[fname] = tempOverlays.size() - 1;
        }
        else
            numDeleted++;
    }
    m_Overlays = tempOverlays;
    m_Filenames = tempMap;

    // Add the new files into the overlay list.
    int numNew = 0;
    for (const auto &filename : sortedImageFiles)
    {
        auto item = m_Filenames.find(filename);
        if (item == m_Filenames.end())
        {
            // If it doesn't already exist in our database:
            ImageOverlay overlay(filename);
            const int size = m_Filenames.size();  // place before push_back().
            m_Overlays.push_back(overlay);
            m_Filenames[filename] = size;
            numNew++;
        }
    }
    updateStatus(QString("%1 overlays (%2 new, %3 deleted) %4 solved")
                 .arg(m_Overlays.size()).arg(numNew).arg(numDeleted).arg(numAvailable()));
    updateGui();
    save();
}

void ImageOverlayComponent::loadAllImageFiles()
{
    QtConcurrent::run(this, &ImageOverlayComponent::loadImageFileLoop);
}

void ImageOverlayComponent::loadImageFileLoop()
{
    updateStatus(QString("Loading image files..."));
    while (loadImageFile());
    int num = 0;
    for (const auto &o : m_Overlays)
        if (o.m_Img.get() != nullptr)
            num++;
    updateStatus(QString("%1 image files loaded.").arg(num));
}

QImage *ImageOverlayComponent::loadImageFile (const QString &fullFilename, bool mirror)
{
    auto img = new QImage(fullFilename);

    // perhaps deal with max for both h and w.
    int scaleWidth = std::min(img->width(), Options::imageOverlayMaxDimension());
    QImage *processedImg = new QImage;
    if (mirror)
        *processedImg = img->mirrored(true, false).scaledToWidth(scaleWidth); // it's reflected horizontally
    else
        *processedImg = img->scaledToWidth(scaleWidth);

    return processedImg;
}

bool ImageOverlayComponent::loadImageFile()
{
    bool updatedSomething = false;

    for (auto &o : m_Overlays)
    {
        if (o.m_Status == o.ImageOverlay::AVAILABLE && o.m_Img.get() == nullptr)
        {
            QString fullFilename = QString("%1%2%3").arg(m_Directory).arg(QDir::separator()).arg(o.m_Filename);
            const bool mirror = !o.m_EastToTheRight;
            QImage *img = loadImageFile(fullFilename, mirror);
            o.m_Img.reset(img);
            updatedSomething = true;

            // Note: we kept the original width and height in o.m_Width/m_Height even
            // though the image was rescaled. This is to get the rendering right
            // with the original scale.
        }
    }
    return updatedSomething;
}


// Copies the info in m_Overlays into m_ImageOverlayTable
void ImageOverlayComponent::updateGui()
{
    // This clears the table.
    setupTable(m_ImageOverlayTable);

    int row = 0;
    for (int i = 0; i < m_Overlays.size(); ++i)
    {
        const ImageOverlay &overlay = m_Overlays[row];
        setupTextRow(m_ImageOverlayTable, row, FILENAME_COL, overlay.m_Filename);

#if 0
        QCheckBox *checkbox = new QCheckBox();
        checkbox->setChecked(overlay.m_Enabled);
        checkbox->setText(checkbox->isChecked() ? "On" : "Off");
        connect(checkbox, &QCheckBox::clicked, this, [row](bool checked)
        {
            fprintf(stderr, "Row %d was changed to %s\n", row, checked ? "ON" : "OFF");

            //////////// add stuff here
        });
        m_ImageOverlayTable->setCellWidget(row, ENABLED_COL, checkbox);

        if (overlay.m_Nickname.size() > 0)
            setupTextRow(m_ImageOverlayTable, row, NICKNAME_COL, overlay.m_Nickname);
#endif

        QString statusStr = StatusNames[static_cast<int>(overlay.m_Status)];
        setupTextRow(m_ImageOverlayTable, row, STATUS_COL, statusStr);

        if (overlay.m_Orientation != ImageOverlay::BAD_FLOAT)
            setupTextRow(m_ImageOverlayTable, row, ORIENTATION_COL, QString("%1").arg(overlay.m_Orientation, 0, 'f', 2));

        if (overlay.m_RA != ImageOverlay::BAD_FLOAT)
            setupTextRow(m_ImageOverlayTable, row, RA_COL, dms(overlay.m_RA).toHMSString());

        if (overlay.m_DEC != ImageOverlay::BAD_FLOAT)
            setupTextRow(m_ImageOverlayTable, row, DEC_COL, dms(overlay.m_DEC).toDMSString());

        if (overlay.m_PixelsPerArcsec != ImageOverlay::BAD_FLOAT)
            setupTextRow(m_ImageOverlayTable, row, ARCSEC_PER_PIXEL_COL, QString("%1").arg(overlay.m_PixelsPerArcsec, 0, 'f', 2));

        setupTextRow(m_ImageOverlayTable, row, EAST_TO_RIGHT_COL,
                     QString("%1").arg(overlay.m_EastToTheRight ? "East-Right" : "West-Right"));

        if (overlay.m_Width != 0)
            setupTextRow(m_ImageOverlayTable, row, WIDTH_COL, QString("%1").arg(overlay.m_Width));

        if (overlay.m_Height != 0)
            setupTextRow(m_ImageOverlayTable, row, HEIGHT_COL, QString("%1").arg(overlay.m_Height));

        row++;
    }
    m_ImageOverlayTable->resizeColumnsToContents();
}

void ImageOverlayComponent::load()
{
    QList<ImageOverlay *> list;
    KStarsData::Instance()->userdb()->GetAllImageOverlays(&m_Overlays);
    // Alphabetize.
    std::sort(m_Overlays.begin(), m_Overlays.end(), overlaySorter);
    m_Filenames.clear();
    int index = 0;
    for (const auto &o : m_Overlays)
    {
        m_Filenames[o.m_Filename] = index;
        index++;
    }
}

void ImageOverlayComponent::save()
{
    KStarsData::Instance()->userdb()->DeleteAllImageOverlays();

    foreach (ImageOverlay metadata, m_Overlays)
        KStarsData::Instance()->userdb()->AddImageOverlay(metadata);
}

void ImageOverlayComponent::solveImage(const QString &filename)
{
    constexpr int solverTimeout = 30;
    auto profiles = Ekos::getDefaultAlignOptionsProfiles();
    auto parameters = profiles.at(Options::solveOptionsProfile());
    // Double search radius
    parameters.search_radius = parameters.search_radius * 2;

    m_Solver.reset(new SolverUtils(parameters, solverTimeout),  &QObject::deleteLater);
    connect(m_Solver.get(), &SolverUtils::done, this, &ImageOverlayComponent::solverDone, Qt::UniqueConnection);
#if 0
    auto lowScale = Options::astrometryImageScaleLow();
    auto highScale = Options::astrometryImageScaleHigh();

    // solver utils uses arcsecs per pixel only
    if (Options::astrometryImageScaleUnits() == SSolver::DEG_WIDTH)
    {
        lowScale = (lowScale * 3600) / std::max(width, height);
        highScale = (highScale * 3600) / std::min(width, height);
    }
    else if (Options::astrometryImageScaleUnits() == SSolver::ARCMIN_WIDTH)
    {
        lowScale = (lowScale * 60) / std::max(width, height);
        highScale = (highScale * 60) / std::min(width, height);
    }

    m_Solver->useScale(Options::astrometryUseImageScale(), lowScale, highScale);
    m_Solver->usePosition(Options::astrometryUsePosition(), currentJob->getTargetCoords().ra().Degrees(),
                          currentJob->getTargetCoords().dec().Degrees());
    ////m_Solver->setHealpix(m_IndexToUse, m_HealpixToUse);
#endif

    if (m_RowsToSolve.size() > 1)
        updateStatus(QString("Solving: %1. %2 in queue.").arg(filename).arg(m_RowsToSolve.size()));
    else
        updateStatus(QString("Solving: %1.").arg(filename));
    m_Solver->runSolver(filename);
}

void ImageOverlayComponent::tryAgain()
{
    m_TryAgainTimer.stop();
    if (m_RowsToSolve.size() > 0)
        startSolving();
}

int ImageOverlayComponent::numAvailable()
{
    int num = 0;
    for (const auto &o : m_Overlays)
        if (o.m_Status == ImageOverlay::AVAILABLE)
            num++;
    return num;
}

void ImageOverlayComponent::show()
{
    auto selections = m_ImageOverlayTable->selectionModel();
    if (selections->hasSelection())
    {
        auto selectedIndexes = selections->selectedIndexes();
        const int row = selectedIndexes.at(0).row();
        if (m_Overlays.size() > row)
        {
            if (m_Overlays[row].m_Status != ImageOverlay::AVAILABLE)
            {
                updateStatus(QString("Can't show %1. Not plate solved.").arg(m_Overlays[row].m_Filename));
                return;
            }
            if (m_Overlays[row].m_Img.get() == nullptr)
            {
                updateStatus(QString("Can't show %1. Image not loaded.").arg(m_Overlays[row].m_Filename));
                return;
            }
            const double ra = m_Overlays[row].m_RA;
            const double dec = m_Overlays[row].m_DEC;

            // Convert the RA/DEC from j2000 to jNow.
            auto localTime = KStarsData::Instance()->geo()->UTtoLT(KStarsData::Instance()->clock()->utc());
            const dms raDms(ra), decDms(dec);
            SkyPoint coord(raDms, decDms);
            coord.apparentCoord(static_cast<long double>(J2000), KStars::Instance()->data()->ut().djd());
            SkyMap::Instance()->setFocus(dms(coord.ra()), dms(coord.dec()));

            // Zoom factor is in pixels per radian.
            double zoomFactor = (400 * 60.0 *  10800.0) / (m_Overlays[row].m_Width * m_Overlays[row].m_PixelsPerArcsec * dms::PI);
            SkyMap::Instance()->setZoomFactor(zoomFactor);

            SkyMap::Instance()->forceUpdate(true);
        }
    }
}

void ImageOverlayComponent::abortSolving()
{
    m_RowsToSolve.clear();
    if (m_Solver)
        m_Solver->abort();
    updateStatus(QString("Solving aborted."));
}

void ImageOverlayComponent::startSolving()
{
    if (m_Solver && m_Solver->isRunning())
    {
        m_Solver->abort();
        if (m_RowsToSolve.size() > 0)
        {
            // updateStatus(QString("Solver still running, retrying in 2s."));
            m_TryAgainTimer.start(2000);
        }
        return;
    }

    if (m_RowsToSolve.size() == 0)
    {
        QSet<int> selectedRows;
        auto selections = m_ImageOverlayTable->selectionModel();
        if (selections->hasSelection())
        {
            // Need to de-dup, as selecting the whole row will select all the columns.
            auto selectedIndexes = selections->selectedIndexes();
            for (int i = 0; i < selectedIndexes.count(); ++i)
            {
                // Don't insert a row that's already solved.
                const int row = selectedIndexes.at(i).row();
                if (m_Overlays[row].m_Status == ImageOverlay::AVAILABLE)
                {
                    updateStatus(QString("Skipping already solved: %1.").arg(m_Overlays[row].m_Filename));
                    continue;
                }
                selectedRows.insert(row);
            }
        }
        m_RowsToSolve.clear();
        for (int row : selectedRows)
            m_RowsToSolve.push_back(row);
    }

    if (m_RowsToSolve.size() > 0)
    {
        const int row = m_RowsToSolve[0];
        const QString filename =
            QString("%1/%2").arg(m_Directory).arg(m_Overlays[row].m_Filename);
        if (m_Overlays[row].m_Status == ImageOverlay::AVAILABLE)
        {
            updateStatus(QString("%1 already solved. Skipping.").arg(filename));
            m_RowsToSolve.removeFirst();
            if (m_RowsToSolve.size() > 0)
                startSolving();
            return;
        }

        auto img = new QImage(filename); // Probably put in another thread?
        m_Overlays[row].m_Width = img->width();
        m_Overlays[row].m_Height = img->height();
        solveImage(filename);
    }
}

void ImageOverlayComponent::reload()
{
    //updateTable();
    // Problem with reload is it can crash kstars if the image load loop is running, or
    // if something else is messing with m_Overlays. Need mutex protection.
    updateStatus(QString("Reload not yet implemented. Currently you need to restart KStars to do this."));
}

void ImageOverlayComponent::solverDone(bool timedOut, bool success, const FITSImage::Solution &solution,
                                       double elapsedSeconds)
{
    disconnect(m_Solver.get(), &SolverUtils::done, this, &ImageOverlayComponent::solverDone);
    if (m_RowsToSolve.size() == 0)
        return;

    const int solverRow = m_RowsToSolve[0];
    m_RowsToSolve.removeFirst();

    if (timedOut)
    {
        updateStatus(QString("Solver timed out in %1s").arg(elapsedSeconds, 0, 'f', 1));
        m_Overlays[solverRow].m_Status = ImageOverlay::PLATE_SOLVE_FAILURE;
    }
    else if (!success)
    {
        m_Overlays[solverRow].m_Status = ImageOverlay::PLATE_SOLVE_FAILURE;
        updateStatus(QString("Solver failed in %1s").arg(elapsedSeconds, 0, 'f', 1));
    }
    else
    {
        m_Overlays[solverRow].m_Orientation = solution.orientation;
        m_Overlays[solverRow].m_RA = solution.ra;
        m_Overlays[solverRow].m_DEC = solution.dec;
        m_Overlays[solverRow].m_PixelsPerArcsec = solution.pixscale;
        m_Overlays[solverRow].m_EastToTheRight = solution.parity;
        m_Overlays[solverRow].m_Status = ImageOverlay::AVAILABLE;

        updateStatus(QString("Solver success in %1s: RA %2 DEC %3 Scale %4 Angle %5")
                     .arg(elapsedSeconds, 0, 'f', 1)
                     .arg(solution.ra, 0, 'f', 2)
                     .arg(solution.dec, 0, 'f', 2)
                     .arg(solution.pixscale, 0, 'f', 2)
                     .arg(solution.orientation, 0, 'f', 2)
                    );

        // Load the image.
        QString fullFilename = QString("%1/%2").arg(m_Directory).arg(m_Overlays[solverRow].m_Filename);
        QImage *img = loadImageFile(fullFilename, !m_Overlays[solverRow].m_EastToTheRight);
        m_Overlays[solverRow].m_Img.reset(img);
    }
    save();
    updateGui();

    if (m_RowsToSolve.size() > 0)
        startSolving();
    else
    {
        updateStatus(QString("Done solving. %1 available.").arg(numAvailable()));
    }
}
