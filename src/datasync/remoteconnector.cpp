#include "remoteconnector_p.h"
#include "logger.h"

using namespace QtDataSync;

#define QTDATASYNC_LOG QTDATASYNC_LOG_CONTROLLER

const QString RemoteConnector::keyRemoteEnabled(QStringLiteral("enabled"));
const QString RemoteConnector::keyRemoteUrl(QStringLiteral("remote/url"));
const QString RemoteConnector::keyAccessKey(QStringLiteral("remote/accessKey"));
const QString RemoteConnector::keyHeaders(QStringLiteral("remote/headers"));
const QString RemoteConnector::keyUserId(QStringLiteral("userId"));

RemoteConnector::RemoteConnector(const Defaults &defaults, QObject *parent) :
	Controller("connector", defaults, parent),
	_cryptoController(new CryptoController(defaults, this)),
	_socket(nullptr),
	_changingConnection(false)
{}

void RemoteConnector::initialize()
{
	_cryptoController->initialize();
	//always "reconnect", because this loads keys etc. and if disabled, also does nothing
	QMetaObject::invokeMethod(this, "reconnect", Qt::QueuedConnection);
}

void RemoteConnector::finalize()
{
	//TODO disconnect socket

	_cryptoController->finalize();
}

void RemoteConnector::reconnect()
{
	if(_socket) {
		if(_socket->state() == QAbstractSocket::UnconnectedState) {
			logDebug() << "Removing unconnected but still not deleted socket";
			_socket->deleteLater();
			_socket = nullptr;
			emit stateChanged(RemoteDisconnected);
			QMetaObject::invokeMethod(this, "reconnect", Qt::QueuedConnection);
		} else if(_socket->state() == QAbstractSocket::ConnectedState) {
			logDebug() << "Closing active connection with server to reconnect";
			_changingConnection = true;
			_socket->close();
			emit stateChanged(RemoteReconnecting);
		} else
			logDebug() << "Delaying reconnect operation. Socket in changing state:" << _socket->state();
	} else {
		emit stateChanged(RemoteReconnecting);
		QUrl remoteUrl;
		if(!checkCanSync(remoteUrl))
			return;

		_socket = new QWebSocket(sValue(keyAccessKey).toString(),
								 QWebSocketProtocol::VersionLatest,
								 this);

		auto conf = defaults().property(Defaults::SslConfiguration).value<QSslConfiguration>();
		if(!conf.isNull())
			_socket->setSslConfiguration(conf);

		connect(_socket, &QWebSocket::connected,
				this, &RemoteConnector::connected);
		connect(_socket, &QWebSocket::binaryMessageReceived,
				this, &RemoteConnector::binaryMessageReceived);
		connect(_socket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
				this, &RemoteConnector::error);
		connect(_socket, &QWebSocket::sslErrors,
				this, &RemoteConnector::sslErrors);
		connect(_socket, &QWebSocket::disconnected,
				this, &RemoteConnector::disconnected,
				Qt::QueuedConnection);

		QNetworkRequest request(remoteUrl);
		request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
		request.setAttribute(QNetworkRequest::HttpPipeliningAllowedAttribute, true);
		request.setAttribute(QNetworkRequest::SpdyAllowedAttribute, true);
		request.setAttribute(QNetworkRequest::HTTP2AllowedAttribute, true);

		auto keys = sValue(keyHeaders).value<QHash<QByteArray, QByteArray>>();
		for(auto it = keys.begin(); it != keys.end(); it++)
			request.setRawHeader(it.key(), it.value());

		_changingConnection = true;
		_socket->open(request);
		logDebug() << "Connecting to remote server...";
	}
}

void RemoteConnector::reloadState()
{

}

void RemoteConnector::connected()
{
	logDebug() << "Successfully connected to remote server";
	emit stateChanged(RemoteLoadingState);
}

void RemoteConnector::disconnected()
{
	if(!_changingConnection) {
		logWarning().noquote() << "Unexpected disconnect from server with exit code"
							   << _socket->closeCode()
							   << "and reason:"
							   << _socket->closeReason();
		//TODO retry
//		auto delta = retry();
//		logDebug() << "Retrying to connect to server in"
//				   << delta / 1000
//				   << "seconds";
	} else {
		_changingConnection = false;
		logDebug() << "Remote server has been disconnected";
	}

	if(_socket)//better be safe
		_socket->deleteLater();
	_socket = nullptr;

	//always "disable" remote for the state changer
	emit stateChanged(RemoteDisconnected);
}

void RemoteConnector::binaryMessageReceived(const QByteArray &message)
{
	try {
		QDataStream stream(message);
		setupStream(stream);
		stream.startTransaction();
		QByteArray name;
		stream >> name;
		if(!stream.commitTransaction())
			throw DataStreamException(stream);

		if(isType<IdentifyMessage>(name))
			onIdentify(deserializeMessage<IdentifyMessage>(stream));
		else
			logWarning() << "Unknown message received: " << message;
	} catch(DataStreamException &e) {
		logCritical() << "Remote message error:" << e.what();
	}
}

void RemoteConnector::error(QAbstractSocket::SocketError error)
{
	//TODO retryIndex
//	if(retryIndex == 0) {
		logWarning().noquote() << "Server connection socket error:"
							   << _socket->errorString();
//	} else {
//		logDebug().noquote() << "Repeated server connection socket error:"
//							 << _socket->errorString();
//	}

	tryClose();
}

void RemoteConnector::sslErrors(const QList<QSslError> &errors)
{
	auto shouldClose = true;
	foreach(auto error, errors) {
		if(error.error() == QSslError::SelfSignedCertificate ||
		   error.error() == QSslError::SelfSignedCertificateInChain)
			shouldClose = shouldClose &&
						  (defaults().property(Defaults::SslConfiguration)
						   .value<QSslConfiguration>()
						   .peerVerifyMode() >= QSslSocket::VerifyPeer);
		//TODO retryIndex
//		if(retryIndex == 0) {
			logWarning().noquote() << "Server connection SSL error:"
								   << error.errorString();
//		} else {
//			logDebug().noquote() << "Repeated server connection SSL error:"
//								 << error.errorString();
//		}
	}

	if(shouldClose)
		tryClose();
}

bool RemoteConnector::checkCanSync(QUrl &remoteUrl)
{
	//check if sync is enabled
	if(!sValue(keyRemoteEnabled).toBool()) {
		logDebug() << "Remote has been disabled. Not connecting";
		emit stateChanged(RemoteDisconnected);
		return false;
	}

	//check if remote is defined
	remoteUrl = sValue(keyRemoteUrl).toUrl();
	if(!remoteUrl.isValid()) {
		logDebug() << "Cannot connect to remote - no URL defined";
		emit stateChanged(RemoteDisconnected);
		return false;
	}

	//load crypto stuff
	if(!loadIdentity()) {
		logCritical() << "Unable to access private keys of user in keystore. Cannot synchronize";
		emit stateChanged(RemoteDisconnected);
		return false;
	}

	return true;
}

bool RemoteConnector::loadIdentity()
{
	if(!_cryptoController->canAccess()) //no keystore -> can neither save nor load...
		return false;

	_userId = sValue(keyUserId).toUuid();
	if(_userId.isNull()) //no user -> nothing to be loaded
		return true;

	return _cryptoController->loadKeyMaterial(_userId);
}

void RemoteConnector::tryClose()
{
	logDebug() << Q_FUNC_INFO << "socket state:" << _socket->state();
	if(_socket && _socket->state() == QAbstractSocket::ConnectedState) {
		_changingConnection = true;
		_socket->close();
	} else
		emit stateChanged(RemoteDisconnected);
}

QVariant RemoteConnector::sValue(const QString &key) const
{
	if(key == keyHeaders) {
		if(settings()->childGroups().contains(keyHeaders)) {
			settings()->beginGroup(keyHeaders);
			auto keys = settings()->childKeys();
			QHash<QByteArray, QByteArray> headers;
			foreach(auto key, keys)
				headers.insert(key.toUtf8(), settings()->value(key).toByteArray());
			settings()->endGroup();
			return QVariant::fromValue(headers);
		}
	} else {
		auto res = settings()->value(key);
		if(res.isValid())
			return res;
	}

	auto config = defaults().property(Defaults::RemoteConfiguration).value<RemoteConfig>();
	if(key == keyRemoteUrl)
		return config.url;
	else if(key == keyAccessKey)
		return config.accessKey;
	else if(key == keyHeaders)
		return QVariant::fromValue(config.headers);
	else if(key == keyRemoteEnabled)
		return true;
	else
		return {};
}

void RemoteConnector::onIdentify(const IdentifyMessage &message)
{
	logDebug() << message;
	if(settings()->contains(keyUserId)) { //login

	} else {
		//TODO create public key
		//TODO send REGISTER message
	}
}
