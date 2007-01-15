/***************************************************************************
                          modcalcapcoord.cpp  -  description
                             -------------------
    begin                : Wed Apr 10 2002
    copyright            : (C) 2002 by Pablo de Vicente
    email                : vicente@oan.es
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include <QTextStream>

#include <klocale.h>
#include <kfiledialog.h>
#include <kmessagebox.h>

#include "modcalcapcoord.h"
#include "kstars.h"
#include "dms.h"
#include "skypoint.h"
#include "kstarsdatetime.h"
#include "finddialog.h"
#include "widgets/dmsbox.h"
#include "libkdeedu/extdate/extdatetimeedit.h"

modCalcApCoord::modCalcApCoord(QWidget *parentSplit) 
: QFrame(parentSplit) {

	setupUi(this);
	showCurrentTime();
	RACat->setDegType(false);
	DecCat->setDegType(true);

	connect( ObjectButton, SIGNAL( clicked() ), this, SLOT( slotObject() ) );
	connect( NowButton, SIGNAL( clicked() ), this, SLOT( showCurrentTime() ) );
	connect( RACat, SIGNAL( editingFinished() ), this, SLOT( slotCompute() ) );
	connect( DecCat, SIGNAL( editingFinished() ), this, SLOT( slotCompute() ) );
	connect( UT, SIGNAL( timeChanged( const QTime & ) ), this, SLOT( slotCompute() ) );
	connect( Date, SIGNAL( dateChanged( const ExtDate & ) ), this, SLOT( slotCompute() ) );

	connect( utCheckBatch, SIGNAL(clicked()), this, SLOT(slotUtCheckedBatch()) );
	connect( dateCheckBatch, SIGNAL(clicked()), this, SLOT(slotDateCheckedBatch()) );
	connect( raCheckBatch, SIGNAL(clicked()), this, SLOT(slotRaCheckedBatch()) );
	connect( decCheckBatch, SIGNAL(clicked()), this, SLOT(slotDecCheckedBatch()) );
	connect( epochCheckBatch, SIGNAL(clicked()), this, SLOT(slotEpochCheckedBatch()) );
	connect( InputButtonBatch, SIGNAL(clicked()), this, SLOT(slotInputFile()) );
	connect( OutputButtonBatch, SIGNAL(clicked()), this, SLOT(slotOutputFile()) );
	connect( runButtonBatch, SIGNAL(clicked()), this, SLOT(slotRunBatch()) );

	show();
}

modCalcApCoord::~modCalcApCoord(){
}

void modCalcApCoord::showCurrentTime (void)
{
	KStarsDateTime dt( KStarsDateTime::currentDateTime() );
	Date->setDate( dt.date() );
	UT->setTime( dt.time() );
	EpochTarget->setText( QString::number( dt.epoch(), 'f', 3 ) );
}

void modCalcApCoord::slotCompute(){
	KStarsDateTime dt( Date->date(), UT->time() );
	long double jd = dt.djd();

	dt.setFromEpoch( EpochCat->value() );
	long double jd0 = dt.djd();

	SkyPoint sp( RACat->createDms(false), DecCat->createDms() );
	sp.apparentCoord(jd0, jd);

	RA->setText( sp.ra()->toHMSString() );
	Dec->setText( sp.dec()->toDMSString() );
}

void modCalcApCoord::slotObject() {
  FindDialog fd( (KStars*)topLevelWidget()->parent() );
  if ( fd.exec() == QDialog::Accepted ) {
    SkyObject *o = fd.currentItem();
    RACat->showInHours( o->ra0() );
    DecCat->showInDegrees( o->dec0() );
    EpochCat->setValue( 2000.0 );
  }
}

void modCalcApCoord::slotUtCheckedBatch(){
	if ( utCheckBatch->isChecked() )
		utBoxBatch->setEnabled( false );
	else {
		utBoxBatch->setEnabled( true );
	}
}

void modCalcApCoord::slotDateCheckedBatch(){

	if ( dateCheckBatch->isChecked() )
		dateBoxBatch->setEnabled( false );
	else {
		dateBoxBatch->setEnabled( true );
	}
}

void modCalcApCoord::slotRaCheckedBatch(){

	if ( raCheckBatch->isChecked() )
		raBoxBatch->setEnabled( false );
	else {
		raBoxBatch->setEnabled( true );
	}
}

void modCalcApCoord::slotDecCheckedBatch(){

	if ( decCheckBatch->isChecked() )
		decBoxBatch->setEnabled( false );
	else {
		decBoxBatch->setEnabled( true );
	}
}

void modCalcApCoord::slotEpochCheckedBatch(){

	if ( epochCheckBatch->isChecked() )
		epochBoxBatch->setEnabled( false );
	else {
		epochBoxBatch->setEnabled( true );
	}
}

void modCalcApCoord::slotInputFile() {
	QString inputFileName;
	inputFileName = KFileDialog::getOpenFileName( );
	InputLineEditBatch->setText( inputFileName );
}

void modCalcApCoord::slotOutputFile() {
	QString outputFileName;
	outputFileName = KFileDialog::getSaveFileName( );
	OutputLineEditBatch->setText( outputFileName );
}

void modCalcApCoord::slotRunBatch() {

	QString inputFileName;

	inputFileName = InputLineEditBatch->text();

	// We open the input file and read its content

	if ( QFile::exists(inputFileName) ) {
		QFile f( inputFileName );
		if ( !f.open( QIODevice::ReadOnly) ) {
			QString message = i18n( "Could not open file %1.", f.fileName() );
			KMessageBox::sorry( 0, message, i18n( "Could Not Open File" ) );
			inputFileName = QString();
			return;
		}

//		processLines(&f);
		QTextStream istream(&f);
		processLines(istream);
//		readFile( istream );
		f.close();
	} else  {
		QString message = i18n( "Invalid file: %1", inputFileName );
		KMessageBox::sorry( 0, message, i18n( "Invalid file" ) );
		inputFileName = QString();
		InputLineEditBatch->setText( inputFileName );
		return;
	}
}

//void modCalcApCoord::processLines( const QFile * fIn ) {
void modCalcApCoord::processLines( QTextStream &istream ) {

	// we open the output file

//	QTextStream istream(&fIn);
	QString outputFileName;
	outputFileName = OutputLineEditBatch->text();
	QFile fOut( outputFileName );
	fOut.open(QIODevice::WriteOnly);
	QTextStream ostream(&fOut);

	QString line;
	QString space = " ";
	int i = 0;
	long double jd, jd0;
	SkyPoint sp;
	QTime utB;
	ExtDate dtB;
	dms raB, decB;
	QString epoch0B;

	while ( ! istream.atEnd() ) {
		line = istream.readLine();
		line.trimmed();

		//Go through the line, looking for parameters

		QStringList fields = line.split( " " );

		i = 0;

		// Read Ut and write in ostream if corresponds

		if(utCheckBatch->isChecked() ) {
			utB = QTime::fromString( fields[i] );
			i++;
		} else
			utB = utBoxBatch->time();

		if ( allRadioBatch->isChecked() )
			ostream << utB.toString() << space;
		else
			if(utCheckBatch->isChecked() )
				ostream << utB.toString() << space;

		// Read date and write in ostream if corresponds

		if(dateCheckBatch->isChecked() ) {
			dtB = ExtDate::fromString( fields[i] );
			i++;
		} else
			dtB = dateBoxBatch->date();

		if ( allRadioBatch->isChecked() )
			ostream << dtB.toString().append(space);
		else
			if(dateCheckBatch->isChecked() )
				ostream << dtB.toString().append(space);

		// Read RA and write in ostream if corresponds

		if(raCheckBatch->isChecked() ) {
			raB = dms::fromString( fields[i],false);
			i++;
		} else
			raB = raBoxBatch->createDms(false);

		if ( allRadioBatch->isChecked() )
			ostream << raB.toHMSString() << space;
		else
			if(raCheckBatch->isChecked() )
				ostream << raB.toHMSString() << space;

		// Read DEC and write in ostream if corresponds

		if(decCheckBatch->isChecked() ) {
			decB = dms::fromString( fields[i], true);
			i++;
		} else
			decB = decBoxBatch->createDms();

		if ( allRadioBatch->isChecked() )
			ostream << decB.toDMSString() << space;
		else
			if(decCheckBatch->isChecked() )
				ostream << decB.toHMSString() << space;

		// Read Epoch and write in ostream if corresponds

		if(epochCheckBatch->isChecked() ) {
			epoch0B = fields[i];
			i++;
		} else
			epoch0B = epochBoxBatch->text();

		if ( allRadioBatch->isChecked() )
			ostream << epoch0B;
		else
			if(decCheckBatch->isChecked() )
				ostream << epoch0B;

		KStarsDateTime dt;
		dt.setFromEpoch( epoch0B );
		jd = KStarsDateTime(dtB,utB).djd();
		jd0 = dt.djd();
		sp = SkyPoint (raB, decB);
		sp.apparentCoord(jd0, jd);

		ostream << sp.ra()->toHMSString() << sp.dec()->toDMSString() << endl;
	}

	fOut.close();
}

#include "modcalcapcoord.moc"
