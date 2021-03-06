/*
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

#include "videocodecsupportinfo.h"

VideoCodecSupportInfo::VideoCodecSupportInfo(QString externalVideoEncoderLocation, QObject* parent)
    : QObject(parent)
{
    m_videoEncoderLocation = externalVideoEncoderLocation;
    m_testFileName = "dummy_Wa8F7bVL3lmF4ngfD0894u32Nd.avi";
    m_isInitialized = false;

    m_rawVideoCodecStr = "IYUV";

    m_codecSupport.insert(stringToFourcc(m_rawVideoCodecStr), QList<int>());
    m_codecSupport.insert(stringToFourcc("FFV1"), QList<int>());
    m_codecSupport.insert(stringToFourcc("LAGS"), QList<int>());

    // These codes are used by external encoder (ffmpeg/avconv). Use parameter -codecs for list.
    m_fourccToEncoderStr.insert(stringToFourcc(m_rawVideoCodecStr), "rawvideo");
    m_fourccToEncoderStr.insert(stringToFourcc("FFV1"), "ffv1");
    m_fourccToEncoderStr.insert(stringToFourcc("LAGS"), "lagarith");

    // These names are shown to the user for codec selection
    m_fourccToCodecName.insert(stringToFourcc(m_rawVideoCodecStr), tr("Raw Video"));
    m_fourccToCodecName.insert(stringToFourcc("FFV1"), tr("FFV1 Lossless Video"));
    m_fourccToCodecName.insert(stringToFourcc("LAGS"), tr("Lagarith Lossless Video"));
}

bool VideoCodecSupportInfo::init() {
    if (m_isInitialized) {
        return false;
    }
    int codec = 0;
    QListIterator<int> codecIt(m_codecSupport.keys());
    QList<int> encoderList;

    while (codecIt.hasNext()) {
        codec = codecIt.next();
        if (testOpencvSupport(codec)) {
            encoderList = m_codecSupport.value(codec);
            encoderList.append(VideoCodecSupportInfo::OpenCv);
            m_codecSupport.insert(codec, encoderList);
        }
        if (testEncoderSupport(codec)) {
            encoderList = m_codecSupport.value(codec);
            encoderList.append(VideoCodecSupportInfo::External);
            m_codecSupport.insert(codec, encoderList);
        }
    }
    m_isInitialized = true;
    return true;
}

bool VideoCodecSupportInfo::isInitialized() {
    return m_isInitialized;
}

int VideoCodecSupportInfo::stringToFourcc(QString fourccStr) {
    if (fourccStr.length() != 4) {
        return 0;
    }
    char* str = fourccStr.toLocal8Bit().data();
    return CV_FOURCC(str[0], str[1], str[2], str[3]);
}

QString VideoCodecSupportInfo::fourccToString(int fourcc) {
    QString str;
    for (int i = 0; i < 4; i++) {
        str.append(QChar((char)((fourcc >> i*8) & 0xff)));
    }
    return str;
}

bool VideoCodecSupportInfo::isOpencvSupported(int fourcc) {
    return m_codecSupport.value(fourcc).contains(VideoCodecSupportInfo::OpenCv);
}

bool VideoCodecSupportInfo::isEncoderSupported(int fourcc) {
    return m_codecSupport.value(fourcc).contains(VideoCodecSupportInfo::External);
}

QList<int> VideoCodecSupportInfo::supportedCodecs() {
    QList<int> codecs;
    QListIterator<int> codecIt(m_codecSupport.keys());
    int codec = 0;

    while (codecIt.hasNext()) {
        codec = codecIt.next();
        if (!m_codecSupport.value(codec).isEmpty()) {
            codecs << codec;
        }
    }
    return codecs;
}

QString VideoCodecSupportInfo::codecName(int fourcc) {
    return m_fourccToCodecName.value(fourcc, "");
}

QString VideoCodecSupportInfo::fourccToEncoderString(int fourcc) {
    return m_fourccToEncoderStr.value(fourcc, "");
}

QString VideoCodecSupportInfo::rawVideoCodecStr() {
    return m_rawVideoCodecStr;
}

void VideoCodecSupportInfo::removeSupport(int fourcc, int encoder) {
    if (m_codecSupport.keys().contains(fourcc)) {
        QList<int> supportList = m_codecSupport.value(fourcc);
        supportList.removeAll(encoder);
        m_codecSupport.insert(fourcc, supportList);
    }
}

bool VideoCodecSupportInfo::testOpencvSupport(int fourcc) {
    cv::VideoWriter writer;
    cv::Mat frame;
    bool supported = false;
    int writtenFourcc = 0;
    std::string testFileNameStd(m_testFileName.toLocal8Bit().data());

    // try opening & writing file
    try {
        if (!writer.open(testFileNameStd, fourcc, 25, cv::Size(640, 480))) {
            return false;
        }
    } catch (cv::Exception& e) {
        return false;
    }
    if (!writer.isOpened()) {
        return false;
    }
    frame = cv::Mat(480, 640, CV_8UC3);
    writer.write(frame);
    writer.release();

    // check result
    cv::VideoCapture reader;
    if (!reader.open(testFileNameStd)) {
        return false;
    }
    writtenFourcc = (int)reader.get(CV_CAP_PROP_FOURCC);
    if (writtenFourcc == fourcc) {
        supported = true;
    }
    QFile testFile(m_testFileName);
    testFile.remove();
    return supported;
}

bool VideoCodecSupportInfo::testEncoderSupport(int fourcc) {
    QProcess encoder;
    QStringList encoderOutput;
    QStringList args;
    QString encoderCodecStr = m_fourccToEncoderStr.value(fourcc);

    if (encoderCodecStr.isEmpty()) {
        return false;
    }

    args << "-codecs";
    encoder.start(m_videoEncoderLocation, args);
    encoder.waitForFinished();
    while (encoder.canReadLine()) {
        encoderOutput << encoder.readLine();
    }
    QStringListIterator listIt(encoderOutput);
    while (listIt.hasNext()) {
        QString line = listIt.next();
        // Find encoder name and verify it can encode videos.
        QRegularExpression regex("^ *.EV...\\ " + encoderCodecStr + ".*$");
        if (regex.match(line).hasMatch()) {
            return true;
        }
    }
    return false;
}

