/*
    SPDX-FileCopyrightText: 2023 Hy Murveit <hy@murveit.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "imageoverlaycomponent.h"
#include "skycomponent.h"
#include <QSharedPointer>
#include <QImage>
#include <QObject>
#include <QLabel>
#include <QTimer>
#include <QPlainTextEdit>
#include "fitsviewer/fitsdata.h"

class QTableWidget;
class SolverUtils;

class ImageOverlay
{
    public:
        enum Status
        {
            UNPROCESSED = 0,
            BAD_FILE,
            PLATE_SOLVE_FAILURE,
            OTHER_ERROR,
            AVAILABLE,
            NUM_STATUS
        };

        ImageOverlay(const QString &filename = "", bool enabled = true, const QString &nickname = "",
                     Status status = UNPROCESSED, double orientation = 0, double ra = 0, double dec = 0,
                     double pixelsPerArcsec = 0, bool eastToTheRight = true, int width = 0, int height = 0)
            : m_Filename(filename), m_Enabled(enabled), m_Nickname(nickname), m_Status(status),
              m_Orientation(orientation), m_RA(ra), m_DEC(dec), m_PixelsPerArcsec(pixelsPerArcsec),
              m_EastToTheRight(eastToTheRight), m_Width(width), m_Height(height)
        {
        }
        static constexpr double BAD_FLOAT = -1000000.0;
        QString m_Filename;
        bool m_Enabled = true;
        QString m_Nickname;
        Status m_Status = UNPROCESSED;
        double m_Orientation = BAD_FLOAT;
        double m_RA = BAD_FLOAT;
        double m_DEC = BAD_FLOAT;
        double m_PixelsPerArcsec = BAD_FLOAT;
        bool m_EastToTheRight = true;
        int m_Width = 0;
        int m_Height = 0;
        QSharedPointer<QImage> m_Img = nullptr;
};

/**
 * @class ImageOverlayComponent
 * Represents the ImageOverlay overlay
 * @author Hy Murveit
 * @version 1.0
 */
class ImageOverlayComponent : public QObject, public SkyComponent
{
    Q_OBJECT

  public:
    explicit ImageOverlayComponent(SkyComposite *);

    virtual ~ImageOverlayComponent() override = default;

    bool selected() override;
    void draw(SkyPainter *skyp) override;
    void setWidgets(QTableWidget *table, QPlainTextEdit *statusDisplay);
    void updateTable();

    const QList<ImageOverlay> imageOverlays() const {
        return m_Overlays;
    }

    public slots:
    void startSolving();
    void abortSolving();
    void show();
    void reload();  // not currently implemented.
    QString directory()
    {
        return m_Directory;
    };

private slots:
    void tryAgain();

private:
    void load();
    void save();
    void solveImage(const QString &filename);
    void solverDone(bool timedOut, bool success, const FITSImage::Solution &solution, double elapsedSeconds);
    void updateGui();
    int numAvailable();
    void updateStatus(const QString &message);

    // Methods that load the image files in the background.
    void loadAllImageFiles();
    void loadImageFileLoop();
    bool loadImageFile();
    QImage *loadImageFile (const QString &fullFilename, bool mirror);


    QTableWidget *m_ImageOverlayTable;
    QPlainTextEdit *m_StatusDisplay;
    QStringList m_LogText;

    QList<ImageOverlay> m_Overlays;
    QMap<QString, int> m_Filenames;
    QSharedPointer<SolverUtils> m_Solver;
    QList<int> m_RowsToSolve;
    QString m_Directory;
    QTimer m_TryAgainTimer;
};
