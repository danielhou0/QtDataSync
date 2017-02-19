#include "client.h"
#include "app.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <QtConcurrent>

Client::Client(DatabaseController *database, QWebSocket *websocket, QObject *parent) :
	QObject(parent),
	database(database),
	socket(websocket),
	clientId(),
	deviceId(),
	socketAddress(socket->peerAddress())
{
	socket->setParent(this);

	connect(socket, &QWebSocket::disconnected,
			this, &Client::deleteLater);
	connect(socket, &QWebSocket::binaryMessageReceived,
			this, &Client::binaryMessageReceived);
	connect(socket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
			this, &Client::error);
	connect(socket, &QWebSocket::sslErrors,
			this, &Client::sslErrors);
}

QUuid Client::userId() const
{
	return clientId;
}

void Client::binaryMessageReceived(const QByteArray &message)
{
	QtConcurrent::run(qApp->threadPool(), [=](){
		QJsonParseError error;
		auto doc = QJsonDocument::fromJson(message, &error);
		if(error.error != QJsonParseError::NoError) {
			qWarning() << "Invalid data received. Parser error:"
					   << error.errorString();
			return;
		}

		auto obj = doc.object();
		auto data = obj["data"];
		if(obj["command"] == QStringLiteral("createIdentity"))
			createIdentity(data.toObject());
		else if(obj["command"] == QStringLiteral("identify"))
			identify(data.toObject());
		else if(obj["command"] == QStringLiteral("loadChanges"))
			loadChanges();
		else if(obj["command"] == QStringLiteral("load"))
			load(data.toObject());
		else if(obj["command"] == QStringLiteral("save"))
			save(data.toObject());
		else if(obj["command"] == QStringLiteral("remove"))
			remove(data.toObject());
		else if(obj["command"] == QStringLiteral("markUnchanged"))
			markUnchanged(data.toObject());
		else {
			qDebug() << "Unknown command"
					 << obj["command"].toString();
			close();
		}
	});
}

void Client::error()
{
	qWarning() << socket->peerAddress()
			   << "Socket error"
			   << socket->errorString();
	socket->close();
}

void Client::sslErrors(const QList<QSslError> &errors)
{
	foreach(auto error, errors) {
		qWarning() << socket->peerAddress()
				   << "SSL errors"
				   << error.errorString();
	}
	socket->close();
}

void Client::createIdentity(const QJsonObject &data)
{
	deviceId = QUuid(data["deviceId"].toString());
	if(deviceId.isNull()) {
		close();
		return;
	}

	auto identity = database->createIdentity(deviceId);
	if(!identity.isNull()) {
		clientId = identity;
		qInfo() << "Created new Identity"
				<< clientId.toByteArray().constData()
				<< "for"
				<< socketAddress;
		sendCommand("identified", clientId.toString());
		emit connected(deviceId, true);
	} else {
		close();
		return;
	}
}

void Client::identify(const QJsonObject &data)
{
	deviceId = QUuid(data["deviceId"].toString());
	if(deviceId.isNull()) {
		close();
		return;
	}
	clientId = QUuid(data["userId"].toString());
	if(clientId.isNull()) {
		close();
		return;
	}

	if(database->identify(clientId, deviceId)) {
		sendCommand("identified", clientId.toString());
		emit connected(deviceId, false);
	} else
		close();
}

void Client::loadChanges()
{
	auto changes = database->loadChanges(clientId, deviceId);
	QJsonObject reply;
	if(!changes.isNull()) {
		reply["success"] = true;
		reply["data"] = changes;
		reply["error"] = QJsonValue::Null;
	} else  {
		reply["success"] = false;
		reply["data"] = QJsonValue::Null;
		reply["error"] = "Failed to load state from server database";
	}
	sendCommand("changeState", reply);
}

void Client::load(const QJsonObject &data)
{
	auto type = data["type"].toString();
	auto key = data["key"].toString();

	QJsonObject reply;
	if(type.isEmpty() || key.isEmpty()) {
		reply["success"] = false;
		reply["data"] = QJsonValue::Null;
		reply["error"] = "Invalid type or key!";
	} else {
		auto replyData = database->load(clientId, type, key);
		if(!replyData.isNull()) {
			reply["success"] = true;
			reply["data"] = replyData;
			reply["error"] = QJsonValue::Null;
		} else  {
			reply["success"] = false;
			reply["data"] = QJsonValue::Null;
			reply["error"] = "Failed to load data from server database";
		}
	}
	sendCommand("completed", reply);
}

void Client::save(const QJsonObject &data)
{
	auto type = data["type"].toString();
	auto key = data["key"].toString();
	auto value = data["value"].toObject();

	QJsonObject reply;
	if(type.isEmpty() || key.isEmpty()) {
		reply["success"] = false;
		reply["error"] = "Invalid type or key!";
	} else if(database->save(clientId, deviceId, type, key, value)) {
		reply["success"] = true;
		reply["error"] = QJsonValue::Null;
	} else  {
		reply["success"] = false;
		reply["error"] = "Failed to save data on server database";
	}
	sendCommand("completed", reply);
}

void Client::remove(const QJsonObject &data)
{
	auto type = data["type"].toString();
	auto key = data["key"].toString();

	QJsonObject reply;
	if(type.isEmpty() || key.isEmpty()) {
		reply["success"] = false;
		reply["error"] = "Invalid type or key!";
	} else if(database->remove(clientId, deviceId, type, key)) {
		reply["success"] = true;
		reply["error"] = QJsonValue::Null;
	} else  {
		reply["success"] = false;
		reply["error"] = "Failed to remove data from server database";
	}
	sendCommand("completed", reply);
}

void Client::markUnchanged(const QJsonObject &data)
{
	auto type = data["type"].toString();
	auto key = data["key"].toString();

	QJsonObject reply;
	if(type.isEmpty() || key.isEmpty()) {
		reply["success"] = false;
		reply["error"] = "Invalid type or key!";
	} else if(database->markUnchanged(clientId, deviceId, type, key)) {
		reply["success"] = true;
		reply["error"] = QJsonValue::Null;
	} else  {
		reply["success"] = false;
		reply["error"] = "Failed to mark as unchanged on server database";
	}
	sendCommand("completed", reply);
}

void Client::close()
{
	QMetaObject::invokeMethod(socket, "close");
}

void Client::sendCommand(const QByteArray &command, const QJsonValue &data)
{
	QJsonObject message;
	message["command"] = QString::fromLatin1(command);
	message["data"] = data;

	QJsonDocument doc(message);
	QMetaObject::invokeMethod(this, "doSend", Q_ARG(QByteArray, doc.toJson(QJsonDocument::Compact)));
}

void Client::doSend(const QByteArray &message)
{
	socket->sendBinaryMessage(message);
}
