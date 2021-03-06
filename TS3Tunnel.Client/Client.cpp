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
#include "Client.h"

#include <QDebug>
#include <QDir>

#include "PlaybackAudioGenerator.h"

const QString Client::PING_STR = "Ping";


int audioStreamCallback(const void *input, void *output, unsigned long frameCount, const PaStreamCallbackTimeInfo* timeInfo, PaStreamCallbackFlags statusFlags, void *userData)
{
	Client::VoiceSession *voiceSession = reinterpret_cast<Client::VoiceSession*>(userData);
	unsigned long audioBufferSize = frameCount * sizeof(opus_int16);

	voiceSession->AudioGenerator->read(reinterpret_cast<char*>(output), audioBufferSize);

	if (voiceSession->SaveEnabled)
	{
		if (voiceSession->AudioSaveFile->isOpen() == false)
		{
			voiceSession->AudioSaveFile->open(QIODevice::OpenModeFlag::WriteOnly);
		}

		voiceSession->AudioSaveFile->write(reinterpret_cast<char*>(output), audioBufferSize);
	}

	if (voiceSession->ListenEnabled == false)
	{
		std::memset(output, 0, audioBufferSize);
	}

	return 0;
}


Client::Client(const QHostAddress &serverAddress, quint16 serverPort, const QString &serverPassword, QObject *parent) : QObject{ parent },
m_serverAddress{ serverAddress },
m_serverPort{ serverPort },
m_serverPassword{ serverPassword },
m_udpSocket{ new QUdpSocket{ this } },
m_serverPingTimer{},
m_decodedVoicePacketsNb{ 0 },
m_decodedVoicePacketsBytesNb{ 0 },
m_decodingErrorsNb{ 0 },
m_voiceSessions{},
m_audioSavePath{ "" }
{
	this->connect(m_udpSocket, SIGNAL(readyRead()), this, SLOT(udpSocket_readyRead()));

	m_serverPingTimer.setInterval(SERVER_PING_TIMER_INTERVAL_SEC * 1000);
	m_serverPingTimer.setSingleShot(false);
	this->connect(&m_serverPingTimer, SIGNAL(timeout()), this, SLOT(serverPingTimer_timeout()));
}

Client::~Client()
{
	for (auto it = m_voiceSessions.begin(); it != m_voiceSessions.end(); ++it)
	{
		Pa_CloseStream((*it)->AudioStream);
		opus_decoder_destroy((*it)->AudioDecoder);
		delete (*it)->AudioGenerator;

		delete *it;
	}

	Pa_Terminate();
}


int Client::getDecodedVoicePacketsNb() const
{
	return m_decodedVoicePacketsNb;
}

int Client::getDecodedVoicePacketsBytesNb() const
{
	return m_decodedVoicePacketsBytesNb;
}

int Client::getDecodingErrorsNb() const
{
	return m_decodingErrorsNb;
}


bool Client::setupAudioPlayback()
{
	PaError paResult = Pa_Initialize();

	if (paResult != paNoError)
	{
		qCritical() << "Error initializing PortAudio: " << paResult;
	}

	return paResult == paNoError;
}

bool Client::registerToServer()
{
	QByteArray registrationData{ m_serverPassword.toStdString().c_str() };
	bool udpSocketBindResult = m_udpSocket->bind(QHostAddress::AnyIPv4);
	qint64 registrationResult = m_udpSocket->writeDatagram(registrationData, m_serverAddress, m_serverPort);

	m_serverPingTimer.start();

	qDebug().nospace() << "Local UDP socket bound on " << m_udpSocket->localAddress() << ":" << m_udpSocket->localPort();

	return udpSocketBindResult == true && registrationResult == registrationData.size();
}


void Client::setAudioSavePath(const QString &path)
{
	m_audioSavePath = path;
}

void Client::setVoiceSessionCapability(quint64 sessionId, VoiceSessionCapability capability, bool enabled)
{
	auto it = m_voiceSessions.find(sessionId);

	if (it != m_voiceSessions.end())
	{
		if (capability == VoiceSessionCapability::Listen)
		{
			(*it)->ListenEnabled = enabled;
		}

		else if (capability == VoiceSessionCapability::Save)
		{
			(*it)->SaveEnabled = enabled;
		}
	}
}


void Client::udpSocket_readyRead()
{
	QByteArray voiceDataBuffer{};
	QDataStream voiceDataStream{ &voiceDataBuffer, QIODevice::ReadOnly };
	QHostAddress senderAddress;
	quint16 senderPort;
	char *voicePacketBuffer = new char[m_udpSocket->pendingDatagramSize()];
	int opusResult = 0;

	voiceDataBuffer.resize(m_udpSocket->pendingDatagramSize());
	m_udpSocket->readDatagram(voiceDataBuffer.data(), voiceDataBuffer.size(), &senderAddress, &senderPort);

	this->decodeVoiceDataStream(voiceDataStream, voiceDataBuffer.size());
}

void Client::serverPingTimer_timeout()
{
	m_udpSocket->writeDatagram(PING_STR.toStdString().c_str(), PING_STR.length(), m_serverAddress, m_serverPort);
}


void Client::decodeVoiceDataStream(QDataStream &voiceDataStream, qint64 voicePacketBufferReserveSize)
{
	char *voicePacketBuffer = new char[voicePacketBufferReserveSize];

	while (!voiceDataStream.atEnd())
	{
		quint16 voicePacketLength = 0;
		quint64 voiceSessionId;
		VoiceSession *voiceSession;
		int opusResult = 0;
		opus_int16 pcm[AUDIO_FRAME_SIZE * OPUS_CHANNEL_COUNT * sizeof(opus_int16)];

		voiceDataStream >> voicePacketLength;
		voiceDataStream >> voiceSessionId;
		voiceDataStream.readRawData(voicePacketBuffer, voicePacketLength);

		voiceSession = this->updateVoiceSessionList(voiceSessionId);

		if (voiceSession->ListenEnabled || voiceSession->SaveEnabled)
		{
			opusResult = opus_decode(voiceSession->AudioDecoder, reinterpret_cast<unsigned char*>(voicePacketBuffer), voicePacketLength, pcm, AUDIO_FRAME_SIZE, 0);

			if (opusResult <= 0)
			{
				m_decodingErrorsNb += 1;
			}

			else
			{
				qint64 decodedBytesNb = opusResult * OPUS_CHANNEL_COUNT * sizeof(opus_int16);

				m_decodedVoicePacketsNb += 1;
				m_decodedVoicePacketsBytesNb += decodedBytesNb;

				voiceSession->AudioGenerator->write(reinterpret_cast<char*>(pcm), decodedBytesNb);
			}
		}
	}
}

Client::VoiceSession *Client::updateVoiceSessionList(quint64 sessionId)
{
	auto it = m_voiceSessions.find(sessionId);

	if (it == m_voiceSessions.end())
	{
		VoiceSession *voiceSession = new VoiceSession{};
		QString audioSaveFilepath = QDir{ m_audioSavePath }.filePath(QString::number(sessionId) + ".pcm");
		int opusResult = 0;

		voiceSession->Id = sessionId;
		voiceSession->AudioDecoder = opus_decoder_create(OPUS_SAMPLE_RATE, OPUS_CHANNEL_COUNT, &opusResult);
		voiceSession->AudioGenerator = new PlaybackAudioGenerator{};
		voiceSession->ListenEnabled = false;
		voiceSession->SaveEnabled = false;
		voiceSession->AudioSaveFile = new QFile{ audioSaveFilepath };

		Pa_OpenDefaultStream(&voiceSession->AudioStream, 0, OPUS_CHANNEL_COUNT, paInt16, OPUS_SAMPLE_RATE, AUDIO_FRAME_SIZE, audioStreamCallback, voiceSession);

		if (opusResult != paNoError || voiceSession->AudioStream == nullptr)
		{
			qCritical() << "Error creating new voice session";
		}

		else
		{
			Pa_StartStream(voiceSession->AudioStream);
		}

		m_voiceSessions.insert(sessionId, voiceSession);

		emit this->newVoiceSession(sessionId);

		return voiceSession;
	}

	return *it;
}
