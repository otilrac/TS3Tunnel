/*
* MIT License
*
* Copyright (c) 2018 Guillaume Truchot - guillaume.truchot@outlook.com
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/
#pragma once

#include <QObject>

#include <opus/opus.h>

#include <portaudio.h>

#include <QBuffer>
#include <QDataStream>
#include <QHash>
#include <QIODevice>
#include <QFile>
#include <QTimer>
#include <QUdpSocket>
#include <QtMultimedia/QAudioOutput>

#include "PlaybackAudioGenerator.h"

class MainWindow;


class Client : public QObject
{
	Q_OBJECT

public:
	enum class VoiceSessionCapability : int
	{
		Listen,
		Save
	};

	struct VoiceSession
	{
		quint64 Id;
		OpusDecoder *AudioDecoder;
		PlaybackAudioGenerator *AudioGenerator;
		PaStream *AudioStream;
		QFile *AudioSaveFile;
		bool ListenEnabled;
		bool SaveEnabled;
	};


	Client(const QHostAddress &serverAddress, quint16 serverPort, const QString &serverPassword, QObject *parent);
	~Client();

	int getDecodedVoicePacketsNb() const;
	int getDecodedVoicePacketsBytesNb() const;
	int getDecodingErrorsNb() const;

	bool setupAudioPlayback();
	bool registerToServer();

	void setAudioSavePath(const QString &path);
	void setVoiceSessionCapability(quint64 sessionId, VoiceSessionCapability capability, bool enabled);


signals:
	void newVoiceSession(quint64 sessionId);


private slots:
	void udpSocket_readyRead();
	void serverPingTimer_timeout();


private:
	void decodeVoiceDataStream(QDataStream &voiceDataStream, qint64 voicePacketBufferReserveSize);
	VoiceSession *updateVoiceSessionList(quint64 sessionId);

	static const int SERVER_PING_TIMER_INTERVAL_SEC = 2;
	static const QString PING_STR;

	static const int OPUS_CHANNEL_COUNT = 1;
	static const std::size_t OPUS_SAMPLE_SIZE = sizeof(opus_int16) * 8;
	static const opus_int32 OPUS_SAMPLE_RATE = 48000;
	static const int AUDIO_FRAME_SIZE = 960;


	QHostAddress m_serverAddress;
	quint16 m_serverPort;
	QString m_serverPassword;
	QUdpSocket *m_udpSocket;
	QTimer m_serverPingTimer;
	int m_decodedVoicePacketsNb;
	int m_decodedVoicePacketsBytesNb;
	int m_decodingErrorsNb;
	QHash<quint64, VoiceSession*> m_voiceSessions;
	QString m_audioSavePath;
};
