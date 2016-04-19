/**
 * UFO Detector | www.UFOID.net
 *
 * Copyright (C) 2016 UFOID
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <iostream>
#include <QTime>
#include <opencv2/imgproc/imgproc.hpp>
#include <QSettings>
#include <QMessageBox>
#include <QDebug>
#include <QListWidgetItem>
#include "clickablelabel.h"
#include "videowidget.h"
#include "camera.h"
#include "settings.h"
#include "imageexplorer.h"
#include <QDesktopServices>
#include <QDir>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkReply>
#include <QByteArray>
#include "uploader.h"
#include <QJsonDocument>
#include <QJsonObject>



using namespace cv;

MainWindow::MainWindow(QWidget *parent, Camera *cameraPtr) :QMainWindow(parent),ui(new Ui::MainWindow),CamPtr(cameraPtr)
{
    ui->setupUi(this);
    qDebug() << "begin constructing mainwindow" ;

    programVersion = "0.6.0";

    this->setWindowTitle("UFO Detector | BETA " + programVersion);

    QSettings mySettings("UFOID","Detector");
    const int WIDTH = mySettings.value("camerawidth",640).toInt();
    const int HEIGHT = mySettings.value("cameraheight",480).toInt();
    const int NOISELEVEL = mySettings.value("noiselevel",2).toInt();
    QString VERSION = mySettings.value("version","").toString();
    int codec;
    if (QSysInfo::windowsVersion()==QSysInfo::WV_WINDOWS8 || QSysInfo::windowsVersion()==QSysInfo::WV_WINDOWS8_1)
	{
        codec = mySettings.value("videocodec",2).toInt();
    }
    else codec = mySettings.value("videocodec",1).toInt();
    const int CODEC = codec;

    if(VERSION==""||VERSION<programVersion)
	{
        mySettings.remove("xmlfile");
        mySettings.setValue("xmlfile",QCoreApplication::applicationDirPath()+"/detectionArea.xml");
        qDebug() << "cleared critical settings";
        mySettings.setValue("version",programVersion);
    }

    ui->sliderNoise->setSliderPosition(NOISELEVEL);
    ui->lineNoise->setText(QString::number(NOISELEVEL));
    isUpdating=true;
    isDetecting=false;
    isRecording=false;
    lastWasPositive=false;
    lastWasInfo=true;
    isValidToken=true;
    counterNegative_=0;
    counterPositive_=0;
    recordingCounter_=0;

    readXmlAndGetRootElement();
    checkAreaXML();
    initializeStylsheet();
    checkFolders();
    if (checkCameraAndCodec(WIDTH,HEIGHT,CODEC))
    {
        threadWebcam.reset(new std::thread(&MainWindow::updateWebcamFrame, this));
    }


    //Add VideoWidget to UI
    QDomNode node = documentXML.firstChildElement().firstChild();
    while( !node.isNull())
	{
        if( node.isElement())
		{
            QDomElement element = node.toElement();
            VideoWidget* mytest = new VideoWidget(this, element.attribute("Pathname", "NULL"), element.attribute("DateTime", "NULL"),element.attribute("Length", "NULL")  );
            connect(mytest->getClickableLabel(), SIGNAL(clicked()),this,SLOT(deletingFileAndRemovingItem()));
            connect(mytest->getUploadLabel(), SIGNAL(clicked()),this,SLOT(createUploadWindow()));
            connect(mytest->getPlayLabel(), SIGNAL(clicked()),this,SLOT(playClip()));
            QListWidgetItem* item = new QListWidgetItem;
            item->setSizeHint(QSize(150,100));
            ui->myList->addItem(item);
            ui->myList->setItemWidget(item,mytest);
        }
        node = node.nextSibling();
    }

    //Check for new version
    manager = new QNetworkAccessManager();
    connect(manager, SIGNAL(finished(QNetworkReply*)), this, SLOT(checkForUpdate(QNetworkReply*)) );
    QNetworkRequest request;
    request.setUrl(QUrl("http://ufoid.net/version.xml"));
    request.setRawHeader( "User-Agent" , "Mozilla Firefox" );
    manager->get(request);


    qDebug() << "mainwindow constructed" ;
}


/*
 * Read webcamframe from webcam
 */
void MainWindow::updateWebcamFrame()
{
    while (isUpdating)
	{
        webcamFrame = CamPtr->getWebcamFrame();
        cv::resize(webcamFrame,webcamFrame, displayResolution,0, 0, INTER_CUBIC);
        cv::cvtColor(webcamFrame, webcamFrame, CV_BGR2RGB);
        QImage qWebcam((uchar*)webcamFrame.data, webcamFrame.cols, webcamFrame.rows, webcamFrame.step, QImage::Format_RGB888);
        emit updatePixmap(qWebcam);
    }
}

void MainWindow::displayPixmap(QImage image)
{
    ui->webcam->setPixmap(QPixmap::fromImage(image));
}

/*
 * Set the signals and slots
 */
void MainWindow::setSignalsAndSlots(ActualDetector* ptrDetector)
{
    theDetector = ptrDetector;
    connect(theDetector,SIGNAL(positiveMessage()),this,SLOT(setPositiveMessage()));
    connect(theDetector,SIGNAL(negativeMessage()),this,SLOT(setNegativeMessage()));
    connect(theDetector,SIGNAL(errorReadingXML()),this,SLOT(setErrorReadingXML()));
    connect(theDetector->getRecorder(),SIGNAL(updateListWidget(QString,QString,QString)),this,SLOT(updateWidgets(QString,QString,QString)));
    connect(theDetector->getRecorder(),SIGNAL(recordingStarted()),this,SLOT(recordingWasStarted()));
    connect(theDetector->getRecorder(),SIGNAL(recordingStopped()),this,SLOT(recordingWasStoped()));
    connect(this,SIGNAL(elementWasRemoved()),theDetector->getRecorder(),SLOT(reloadXML()));
    connect(theDetector, SIGNAL(progressValueChanged(int)), this , SLOT(on_progressBar_valueChanged(int)) );
	connect(theDetector, SIGNAL(broadcastOutputText(QString)), this, SLOT(update_output_text(QString)) );
    connect(this,SIGNAL(updatePixmap(QImage)),this,SLOT(displayPixmap(QImage)));
}

/*
 * Add new VideoWidget element
 */
void MainWindow::updateWidgets(QString filename, QString datetime, QString videoLength)
{
    VideoWidget* newWidget = new VideoWidget(this, filename, datetime, videoLength);
    connect(newWidget->getClickableLabel(), SIGNAL(clicked()),this,SLOT(deletingFileAndRemovingItem()));
    connect(newWidget->getUploadLabel(), SIGNAL(clicked()),this,SLOT(createUploadWindow()));
    connect(newWidget->getPlayLabel(), SIGNAL(clicked()),this,SLOT(playClip()));
    QListWidgetItem* item = new QListWidgetItem;
    item->setSizeHint(QSize(150,100));
    ui->myList->addItem(item);
    ui->myList->setItemWidget(item,newWidget);

    recordingCounter_++;
    ui->lineCount->setText(QString::number(recordingCounter_));
    readXmlAndGetRootElement();
}

/*
 * Play a video from the VideoWidget
 */
void MainWindow::playClip()
{
    if(!isRecording){
        for(int row = 0; row < ui->myList->count(); row++)
		{
            QListWidgetItem *item = ui->myList->item(row);
            VideoWidget* widget = qobject_cast<VideoWidget*>(ui->myList->itemWidget(item));

            if(widget->getPlayLabel()==sender())
			{
                QDesktopServices::openUrl(QUrl::fromUserInput(widget->getPathname()));
            }
        }
    }
    else
	{
        QMessageBox::warning(this,"Error","Playing video file while recording another video is disabled. Wait until the recording has finished");
    }
}

/*
 * Delete video file and remove VideoWidget element from list
 */
void MainWindow::deletingFileAndRemovingItem()
{
    QString dateToRemove;
    for(int row = 0; row < ui->myList->count(); row++)
	{
        QListWidgetItem *item = ui->myList->item(row);
        VideoWidget* widget = qobject_cast<VideoWidget*>(ui->myList->itemWidget(item));

        if(widget->getClickableLabel()==sender())
		{
            qDebug() << "remove widget " << widget->getDateTime();
            dateToRemove = widget->getDateTime();
            QListWidgetItem *itemToRemove = ui->myList->takeItem(row);
            ui->myList->removeItemWidget(itemToRemove);
            QFile::remove(widget->getPathname());
        }
    }

    QDomNode node = documentXML.firstChildElement().firstChild();
    while( !node.isNull())
	{
        if( node.isElement())
		{
            QDomElement element = node.toElement();
            QString dateInXML = element.attribute("DateTime");
            if (dateInXML.compare(dateToRemove)==0)
			{
              documentXML.firstChildElement().removeChild(node);
              if(!fileXML.open(QIODevice::WriteOnly | QIODevice::Text))
			  {
                  qDebug() <<  "fail open xml - delete";
                  return;
              }
              else
			  {
                  QTextStream stream(&fileXML);
                  stream.setCodec("UTF-8");
                  stream << documentXML.toString();
                  fileXML.close();
              }
              break;
            }
        }
        node = node.nextSibling();
    }
    fileXML.close();
    emit elementWasRemoved();

}


void MainWindow::checkToken(QNetworkReply* reply)
{

    qDebug() << "called API";

    if(reply->error() == QNetworkReply::NoError)
    {
        QString strReply = (QString)reply->readAll();
        QJsonDocument jsonResponse = QJsonDocument::fromJson(strReply.toUtf8());
        QString method = jsonResponse.object().value("method").toString();
        if (method == "ftp")
        {
            isValidToken = true;
            qDebug() << "valid";
        }

    }
    else
    {
        qDebug() << reply->error();
    }

    delete reply;
    manager->deleteLater();
    manager = nullptr;
}

/*
 * Display the Upload Window
 */
void MainWindow::createUploadWindow()
{
    if(isValidToken)
    {
        for(int row = 0; row < ui->myList->count(); row++)
        {
            QListWidgetItem *item = ui->myList->item(row);
            VideoWidget* widget = qobject_cast<VideoWidget*>(ui->myList->itemWidget(item));
            if(widget->getUploadLabel()==sender())
            {
                Uploader* upload = new Uploader(this,widget->getPathname());
                upload->show();
                upload->setAttribute(Qt::WA_DeleteOnClose);
            }
        }
    }
    else
    {
        QMessageBox::information(this,"Information","You need a free UFOID.net account to share your videos. If you already have an account enter your User Token in the settings of the UFO Detector");
    }

}

/*
 * Set positive message from ActualDetector
 */
void MainWindow::setPositiveMessage()
{
    QTime time = QTime::currentTime();
    QString message = time.toString();

    if (!lastWasInfo)
	{
        ui->outputText->moveCursor( QTextCursor::End, QTextCursor::MoveAnchor );
        ui->outputText->moveCursor( QTextCursor::StartOfLine, QTextCursor::MoveAnchor );
        ui->outputText->moveCursor( QTextCursor::End, QTextCursor::KeepAnchor );
        ui->outputText->textCursor().removeSelectedText();
        ui->outputText->textCursor().deletePreviousChar();
        ui->outputText->append(message + " - " + "Positive: " + QString::number(++counterPositive_) + " Negative: " + QString::number(counterNegative_));
    }
    else ui->outputText->append(message + " - " + "Positive detection " + QString::number(++counterPositive_));
    lastWasPositive=true;
    lastWasInfo=false;

}

/*
 * Set negative message from ActualDetector
 */
void MainWindow::setNegativeMessage()
{
    QTime time = QTime::currentTime();
    QString message = time.toString();

    if (!lastWasInfo)
	{
        ui->outputText->moveCursor( QTextCursor::End, QTextCursor::MoveAnchor );
        ui->outputText->moveCursor( QTextCursor::StartOfLine, QTextCursor::MoveAnchor );
        ui->outputText->moveCursor( QTextCursor::End, QTextCursor::KeepAnchor );
        ui->outputText->textCursor().removeSelectedText();
        ui->outputText->textCursor().deletePreviousChar();
        ui->outputText->append(message + " - " + "Positive: " + QString::number(counterPositive_) + " Negative: " + QString::number(++counterNegative_));
    }
    else ui->outputText->append(message + " - " + "Negative detection " + QString::number(++counterNegative_));
    lastWasPositive=false;
    lastWasInfo=false;
}

/*
 * Error when reading detection area file
 */
void MainWindow::setErrorReadingXML()
{
    ui->outputText->append("ERROR: xml file containing the area information was not read correctly");
    ui->outputText->append("Detection is not working. Select area of detection first in the settings");
}

/*
 * Set messages from ActualDetector
 */
void MainWindow::addOutputText(QString msg)
{
    QTime time = QTime::currentTime();
	QString output_text(time.toString() + " - " + msg);
    ui->outputText->append(output_text);
    lastWasInfo=true;
    counterNegative_=0;
    counterPositive_=0;
}


void MainWindow::on_sliderNoise_sliderMoved(int position)
{
    ui->lineNoise->setText(QString::number(position));
    theDetector->setNoiseLevel(position);
}

void MainWindow::on_sliderThresh_sliderMoved(int position)
{
    ui->lineThresh->setText(QString::number(position));
    theDetector->setThresholdLevel(position);
}

void MainWindow::on_settingsButton_clicked()
{
    if (!isDetecting)
	{
        settingsDialog = new Settings(0,CamPtr);
        settingsDialog->setModal(true);
        settingsDialog->show();
        settingsDialog->setAttribute(Qt::WA_DeleteOnClose);
    }
    else
	{
        QMessageBox::warning(this,"Error","Stop the detecting process first");
    }    
}


void MainWindow::on_startButton_clicked()
{
    ui->progressBar->show();
    ui->progressBar->repaint();

    if(theDetector->start())
	{
        theDetector->setNoiseLevel(ui->sliderNoise->value());
        theDetector->setThresholdLevel(ui->sliderThresh->value());
        isUpdating = false;
        if (threadWebcam)
		{
			threadWebcam->join(); threadWebcam.reset(); 
		}
        disconnect(this,SIGNAL(updatePixmap(QImage)),this,SLOT(displayPixmap(QImage)));
        connect(theDetector,SIGNAL(updatePixmap(QImage)),this,SLOT(displayPixmap(QImage)));
        isDetecting=true;
        ui->statusLabel->setStyleSheet("QLabel { color : green; }");
        ui->statusLabel->setText("Detection started on " + QTime::currentTime().toString());
        ui->progressBar->hide();
    }
    else ui->statusLabel->setText("Failed to start");

}

void MainWindow::on_stopButton_clicked()
{
    theDetector->stopThread();
    ui->statusLabel->setStyleSheet("QLabel { color : red; }");
    ui->statusLabel->setText("Detection not running");
    isDetecting=false;
    disconnect(theDetector,SIGNAL(updatePixmap(QImage)),this,SLOT(displayPixmap(QImage)));
    connect(this,SIGNAL(updatePixmap(QImage)),this,SLOT(displayPixmap(QImage)));
    if (!threadWebcam)
	{
        isUpdating=true;
        threadWebcam.reset(new std::thread(&MainWindow::updateWebcamFrame, this));
    }
}

void MainWindow::on_checkBox_stateChanged(int arg1)
{
    if(arg1==0){theDetector->willDisplayImage = false;}
    if(arg1==2){theDetector->willDisplayImage = true;}
}

void MainWindow::on_buttonClear_clicked()
{
    ui->outputText->clear();
}


void MainWindow::on_recordingTestButton_clicked()
{
    theDetector->startRecording();
}

void MainWindow::on_aboutButton_clicked()
{
    QMessageBox::information(this,"About",QString("UFO Detector | Beta " + programVersion + "\nwww.UFOID.net \ncontact@ufoid.net"));
}

void MainWindow::on_buttonImageExpl_clicked()
{
    ImageExplorer* imageExpl = new ImageExplorer();
    imageExpl->setModal(true);
    imageExpl->show();
    imageExpl->setAttribute(Qt::WA_DeleteOnClose);
}

/*
 * Check resolution and change UI to fit around the webcam frame
 * return false if incorrect aspect ratio
 */
bool MainWindow::checkAndSetResolution(const int WIDTH, const int HEIGHT)
{
    int aspectRatio = (double)WIDTH/HEIGHT * 10000;
    //cout << aspectRatio << endl;
    if (aspectRatio==17777)
	{
        ui->webcam->resize(QSize(640,360));
        this->setFixedSize(1060,620);
        displayResolution=Size(640,360);
        return true;
    }
    else if (aspectRatio==13333)
	{
        ui->webcam->resize(QSize(640,480));
        displayResolution=Size(640,480);
        this->setFixedSize(1060,740);
        ui->outputText->move(395,570);
        ui->statusLabel->move(270,505);
        ui->buttonClear->move(275,605);
        ui->checkBox->move(395,545);
        ui->startButton->move(945,610);
        ui->stopButton->move(945,645);
        ui->recordingTestButton->move(275,560);
        ui->buttonImageExpl->move(275,640);
        ui->myList->resize(QSize(240,686));
        ui->progressBar->move(580,505);        
        ui->sliderNoise->resize(ui->sliderNoise->width(),130);
        ui->lineNoise->move(ui->lineNoise->x(),ui->lineNoise->y()+42);
        ui->label_8->move(ui->label_8->x(),ui->label_8->y()+40);
        ui->label_9->move(ui->label_9->x(),ui->label_9->y()+40);
        ui->toolButtonNoise->move(ui->toolButtonNoise->x(),ui->toolButtonNoise->y()+42);
        ui->sliderThresh->move(ui->sliderThresh->x(),ui->sliderThresh->y()+40);
        ui->sliderThresh->resize(ui->sliderThresh->width(),130);
        ui->label_10->move(ui->label_10->x(),ui->label_10->y()+83);
        ui->label_11->move(ui->label_11->x(),ui->label_11->y()+83);
        ui->toolButtonThresh->move(ui->toolButtonThresh->x(),ui->toolButtonThresh->y()+83);
        ui->lineThresh->move(ui->lineThresh->x(),ui->lineThresh->y()+85);
        ui->lineCount->move(ui->lineCount->x(),ui->lineCount->y()+85);
        ui->label_2->move(ui->label_2->x(),ui->label_2->y()+85);
        ui->label_3->move(ui->label_3->x(),ui->label_3->y()+85);
        return true;
    }
    else if (aspectRatio==15000)
	{
        ui->webcam->resize(QSize(480,320));
        displayResolution=Size(480,320);
        return true;
    }
    else
	{
        ui->outputText->append("ERROR: Wrong webcam resolution");
        return false;
    }
    return false;
}


/*
 * Get the DisplayImages checkbox state for the ActualDetector
 */
bool MainWindow::getCheckboxState()
{
    return ui->checkBox->isChecked();
}

/*
 * Set Stylesheet for UI
 */
void MainWindow::initializeStylsheet()
{
    this->setStyleSheet("background-color:#515C65; color: white");
    ui->buttonClear->setStyleSheet("background-color:#3C4A62;");
    ui->recordingTestButton->hide();
    ui->progressBar->hide();
    ui->aboutButton->setStyleSheet("background-color:#3C4A62;");
    ui->stopButton->setStyleSheet("background-color:#3C4A62;");
    ui->startButton->setStyleSheet("background-color:#3C4A62;");
    ui->settingsButton->setStyleSheet("background-color:#3C4A62;");
    ui->outputText->setStyleSheet("background-color:#3C4A62;");
    ui->lineCount->setStyleSheet("background-color:#3C4A62;");
    ui->lineNoise->setStyleSheet("background-color:#3C4A62;");
    ui->lineThresh->setStyleSheet("background-color:#3C4A62;");
    ui->toolButtonNoise->setStyleSheet("background-color:#3C4A62;");
    ui->toolButtonThresh->setStyleSheet("background-color:#3C4A62;");
    ui->buttonImageExpl->setStyleSheet("background-color:#3C4A62;");
    ui->checkBox->setStyleSheet("QToolTip { color: #3C4A62; }");
    ui->outputText->append("For feedback and bug reports contact the developer at contact@ufoid.net");
}


/*
 * Read logfile containing existing video infos
 */
void MainWindow::readXmlAndGetRootElement()
{
	fileXML.setFileName(QString(QCoreApplication::applicationDirPath()+"/logs.xml"));
	if(fileXML.exists())
	{
		if(!fileXML.open(QIODevice::ReadOnly | QIODevice::Text))
		{
			qDebug() << "fail reading the file" << endl;
		}
		else
		{
			if(!documentXML.setContent(&fileXML))
			{
				qDebug() << "failed to load doc" << endl;
			}
			else
			{
				fileXML.close();
				qDebug() << "correctly loaded root element" << endl;
			}
		}
	}
	else
	{
		if(!fileXML.open(QIODevice::WriteOnly | QIODevice::Text))
		{
			qDebug() << "failed creating file" << endl;
		}
		else
		{
			qDebug() << "creating xmlfile" << endl;
			QDomDocument tempFirstTime;
            tempFirstTime.appendChild(tempFirstTime.createElement("UFOID"));
			QTextStream stream(&fileXML);
			stream << tempFirstTime.toString();
			fileXML.close();
			readXmlAndGetRootElement();
		}
	}
}

/*
 * Check the detection area file
 */
void MainWindow::checkAreaXML()
{
    QSettings settings("UFOID","Detector");
    String xmlFile = settings.value("xmlfile").toString().toStdString();
    if(xmlFile == "")
	{
        QFile area(QString(QCoreApplication::applicationDirPath()+"/detectionArea.xml"));
        area.open(QIODevice::WriteOnly | QIODevice::Text);
        settings.setValue("xmlfile",QString(QCoreApplication::applicationDirPath()+"/detectionArea.xml"));
        qDebug() << "created areaXML file in " << area.fileName();
    }
    else
	{
        QFile area(xmlFile.c_str());
        if(area.exists())
		{
             qDebug() << "found areaXML file";
        }
        else
		{
            settings.setValue("xmlfile",QString(QCoreApplication::applicationDirPath()+"/detectionArea.xml"));
            area.setFileName(QString(QCoreApplication::applicationDirPath()+"/detectionArea.xml"));
            area.open(QIODevice::WriteOnly | QIODevice::Text);
            qDebug() << "created areaXML file";
        }
    }

}

/*
 * Check camera and codec
 */
bool MainWindow::checkCameraAndCodec(const int WIDTH, const int HEIGHT, const int CODEC)
{
	bool sucess = false;
	if (checkAndSetResolution(WIDTH,HEIGHT)&&!threadWebcam&&CamPtr->isWebcamOpen())
	{
		try
		{
			webcamFrame = CamPtr->getWebcamFrame();
			cv::resize(webcamFrame,webcamFrame, displayResolution,0, 0, INTER_CUBIC);
			sucess = true;
		}
		catch( cv::Exception& e )  
		{
			const char* err_msg = e.what();
			std::cout << "exception caught: " << err_msg << std::endl;
			CamPtr->stopReadingWebcam();
			ui->outputText->append("ERROR: Found webcam but video frame could not be read. Reconnect and check resolution in settings");
		}
	}
	else if (!CamPtr->isWebcamOpen())
	{
		ui->outputText->append("ERROR: Could not open webcam. Select webcam in settings");
	}

	if (CODEC==0)
	{
		VideoWriter theVideoWriter;
		theVideoWriter.open("filename.avi",0, 25,Size(WIDTH,HEIGHT), true);
		if (!theVideoWriter.isOpened())
		{
			ui->outputText->append("ERROR: Could not find Raw Video Codec");
			ui->outputText->append("Please contact the developer with the information about your system");
		}
		else
		{
			theVideoWriter.release();
			remove("filename.avi");
		}
	}
	else if (CODEC==1)
	{
		VideoWriter theVideoWriter;
		theVideoWriter.open("filename.avi",0, 25,Size(WIDTH,HEIGHT), true);
		if (!theVideoWriter.isOpened())
		{
			ui->outputText->append("ERROR: Could not find Raw Video Codec");
			ui->outputText->append("Please contact the developer with the information about your system");
		}
		else
		{
			theVideoWriter.release();
			remove("filename.avi");
		}

		QFile ffmpegFile(QCoreApplication::applicationDirPath()+"/ffmpeg.exe");
		if(!ffmpegFile.exists())
		{
			ui->outputText->append("ERROR: Could not find FFmpeg.exe needed for FFV1 Codec. Please contact the developer.");
		}

	}
	else if (CODEC==2)
	{
		VideoWriter theVideoWriter;
		theVideoWriter.open("filename.avi",CV_FOURCC('L','A','G','S'), 25,Size(WIDTH,HEIGHT), true);
		if (!theVideoWriter.isOpened())
		{
			ui->outputText->append("ERROR: Could not find Lagarith Lossless Video Codec");
			ui->outputText->append("Download and install from http://lags.leetcode.net/codec.html");
		}
		else
		{
			theVideoWriter.release();
			remove("filename.avi");
		}

	}
	
	return sucess;
}

/*
 * Check that the folder for the images and videos exsist
 */
void MainWindow::checkFolders()
{
    QSettings mySettings("UFOID","Detector");
    QString videoFolder = mySettings.value("videofilepath").toString();
    QDir folder(videoFolder);
	
    if (!(folder.exists() &&  videoFolder!=""))
	{
		qDebug() << "create folders";
        QString loc = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)+"/UFO ID";
		folder.mkpath(loc+"/Images");
		folder.mkpath(loc+"/Videos");
		mySettings.setValue("videofilepath",loc+"/Videos");
		mySettings.setValue("imagespath",loc+"/Images");
    }

}

/*
 * Check if xml indicates that there is a new version of the program. Validate userToken if necessary
 */
void MainWindow::checkForUpdate(QNetworkReply *reply)
{
    QByteArray data = reply->readAll();
    qDebug() <<   "XML download size:" << data.size() << "bytes";
    QDomDocument versionXML;

    if(!versionXML.setContent(data))
	{
        qWarning() << "Failed to parse QML";
    }
    else
	{
        QDomElement root;
        root=versionXML.firstChildElement();
        QString versionInXML;
        std::queue<QString> messageInXML;

        QDomNode node = root.firstChild();
        while( !node.isNull())
		{
            if( node.isElement())
			{
                if(node.nodeName()=="version")
				{
                    QDomElement element = node.toElement();
                    versionInXML=element.text();
                }
                if(node.nodeName()=="message")
				{
                    QDomElement element = node.toElement();
                    messageInXML.push(element.text());
                }
            }
            node = node.nextSibling();
        }

        if(versionInXML>programVersion)
		{
            qDebug() << messageInXML.size();
            updateWindow = new MessageUpdate(this,versionInXML,messageInXML);
            updateWindow->show();
            updateWindow->setAttribute(Qt::WA_DeleteOnClose);
        }
    }
	
    delete reply;
    reply = nullptr;

}

void MainWindow::recordingWasStarted()
{
    isRecording=true;
}

void MainWindow::recordingWasStoped()
{
    isRecording=false;
}

MainWindow::~MainWindow()
{
    qDebug() << "Deconstructing MainWindow" ;
    delete ui;
}

/*
 * Stop detecting and save settings when closing application
 */
void MainWindow::closeEvent(QCloseEvent *event)
{
    theDetector->stopThread();

    QSettings theSettings("UFOID","Detector");
    theSettings.setValue("noiselevel",ui->sliderNoise->value());
    isUpdating = false;
    if (threadWebcam)
	{
		threadWebcam->join(); threadWebcam.reset();
	}
    CamPtr->stopReadingWebcam();
    QApplication::quit();
}

void MainWindow::on_progressBar_valueChanged(int value)
{
    ui->progressBar->setValue(value);
}

void MainWindow::update_output_text(QString msg)
{
	addOutputText(msg);
}

void MainWindow::on_toolButtonNoise_clicked()
{
    QMessageBox::information(this,"Information","Select the pixel size of noise that will be ignored in the detection. \nRecommended selection: 2");
}

void MainWindow::on_toolButtonThresh_clicked()
{
    QMessageBox::information(this,"Information","Select the threshold filter value that will be used in the motion detection algorithm. \nIncrease the value if clouds are being detected\nRecommended selection: 10");
}
