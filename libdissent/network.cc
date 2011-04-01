/* libdissent/network.cc
   Network layer (w/ signing and logging) for dissent protocol.

   Author: Shu-Chun Weng <scweng _AT_ cs .DOT. yale *DOT* edu>
 */
/* ====================================================================
 * Dissent: Accountable Group Anonymity
 * Copyright (c) 2010 Yale University.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to
 *
 *   Free Software Foundation, Inc.,
 *   51 Franklin Street, Fifth Floor,
 *   Boston, MA  02110-1301  USA
 */

#include "network.hpp"

#include <QtGlobal>
#include <QAbstractSocket>
#include <QHostAddress>
#include <QSignalMapper>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QVariant>

#include "QByteArrayUtil.hpp"
#include "config.hpp"
#include "crypto.hpp"
#include "random_util.hpp"

namespace Dissent{
Network::Network(Configuration* config)
    : _config(config),
      _inReceivingPhase(false){
    _prepare = new NetworkPrepare(config, &_server, &_clients);

    bool r = _prepare->DoPrepare(
            QHostAddress::Any,
            config->nodes[config->my_node_id].port);
    Q_ASSERT_X(r, "Network::Network(Configuration*)",
               _server.errorString().toLocal8Bit().data());
}

int Network::Send(int node_id, const QByteArray& data){
    // TODO(scw): add nonce and accumulated hash
    QByteArray sig;
    bool r = Crypto::GetInstance()->Sign(&_config->identity_sk,
                                         data, &sig);
    Q_ASSERT_X(r, "Network::Send", "message signing failed");

    // TODO(scw): send msg & signature
    // TODO(scw): log
    return 0;
    (void) node_id;
}

int Network::Broadcast(const QByteArray& data){
    // TODO(scw): add nonce and accumulated hash
    QByteArray sig;
    bool r = Crypto::GetInstance()->Sign(&_config->identity_sk,
                                         data, &sig);
    Q_ASSERT_X(r, "Network::Broadcast", "message signing failed");

    // TODO(scw): send msg & signature
    // TODO(scw): log
    return 0;
}

int Network::Read(int node_id, QByteArray* data){
    // TODO(scw)
    // TODO(scw): filter out message from excluded nodes
    return 0;
    (void) node_id;
    (void) data;
}

void Network::NetworkReady(){
    _signalMapper = new QSignalMapper(this);

    // TODO(scw): fill in _clientNodeId, connect _signalMapper,
    //            destroy _prepare, fire networkReady
}

void Network::ClientHasReadyRead(int node_id){
    QMap<int, QTcpSocket*>::const_iterator it = _clients.constFind(node_id);
    if(it == _clients.constEnd())
        qFatal("Unknown client notifying ready");

    // TODO(scw): buffer input, check signature, then enqueue
}

void Network::StartIncomingNetwork(){
    if(_inReceivingPhase)
        return;

    _inReceivingPhase = true;
    for(QQueue<int>::const_iterator it = _readyQueue.constBegin();
        it != _readyQueue.constEnd(); ++it)
        emit readyRead(_log.at(*it).node_id);
}

void Network::StopIncomingNetwork(){
    _inReceivingPhase = false;
}

const char* const NetworkPrepare::ChallengePropertyName =
    "NetworkPrepareChallenge";
const char* const NetworkPrepare::NodeIdPropertyName =
    "NetworkPrepareNodeId";
const char* const NetworkPrepare::AnswerLengthPropertyName =
    "NetworkPrepareAnswerLength";
const int NetworkPrepare::ChallengeLength = 64;  // SHA-1 uses 512-bit blocks

NetworkPrepare::NetworkPrepare(Configuration* config,
                               QTcpServer* server,
                               QMap<int, QTcpSocket*>* sockets)
    : _config(config), _server(server), _sockets(sockets) {}

bool NetworkPrepare::DoPrepare(const QHostAddress & address, quint16 port){
    connect(_server, SIGNAL(newConnection()),
            this, SLOT(NewConnection()));
    if(!_server->listen(address, port))
        return false;

    _incomeSignalMapper = new QSignalMapper(this);
    connect(_incomeSignalMapper, SIGNAL(mapped(QObject*)),
            this, SLOT(ReadNodeId(QObject*)));
    _answerSignalMapper = new QSignalMapper(this);
    connect(_answerSignalMapper, SIGNAL(mapped(QObject*)),
            this, SLOT(ReadChallengeAnswer(QObject*)));

    _connectSignalMapper = new QSignalMapper(this);
    _errorSignalMapper = new QSignalMapper(this);
    _challengeSignalMapper = new QSignalMapper(this);
    connect(_challengeSignalMapper, SIGNAL(mapped(QObject*)),
            this, SLOT(ReadChallenge(QObject*)));

    QTimer::singleShot(1000, this, SLOT(TryConnect()));
    return true;
}

void NetworkPrepare::AddSocket(int node_id, QTcpSocket* socket){
    _sockets->insert(node_id, socket);
    if(_sockets->size() < _config->nodes.size())
        return;

    for(QMap<int, NodeInfo>::const_iterator it = _config->nodes.constBegin();
        it != _config->nodes.constEnd(); ++it){
        QMap<int, QTcpSocket*>::const_iterator jt =
            _sockets->constFind(it.key());
        if(jt == _sockets->constEnd())
            return;
        QTcpSocket* s = *jt;
        if(!s->isValid() || s->state() != QAbstractSocket::ConnectedState)
            return;
    }

    emit networkReady();
}

void NetworkPrepare::NewConnection(){
    QTcpSocket* socket = _server->nextPendingConnection();

    char challenge[ChallengeLength];
    Random::GetInstance()->GetBlock(sizeof(challenge), challenge);
    QByteArray ba(challenge, sizeof(challenge));
    socket->setProperty(ChallengePropertyName, ba);

    _incomeSignalMapper->setMapping(socket, socket);
    connect(socket, SIGNAL(readyRead()), _incomeSignalMapper, SLOT(map()));

    socket->write(ba);
}

void NetworkPrepare::ReadNodeId(QObject* o){
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(o);
    Q_ASSERT(socket);

    if(socket->bytesAvailable() < 4 + 4)
        return;

    QByteArray data = socket->read(8);
    int node_id = QByteArrayUtil::ExtractInt(true, &data);
    int answer_length = QByteArrayUtil::ExtractInt(true, &data);

    if(socket->peerAddress() != QHostAddress(_config->nodes[node_id].addr)){
        // XXX(scw): wrong host message
        socket->disconnectFromHost();
        delete socket;
        return;
    }

    socket->setProperty(NodeIdPropertyName, node_id);
    socket->setProperty(AnswerLengthPropertyName, answer_length);
    _answerSignalMapper->setMapping(socket, socket);

    disconnect(socket, SIGNAL(readyRead()), _incomeSignalMapper, SLOT(map()));
    connect(socket, SIGNAL(readyRead()), _answerSignalMapper, SLOT(map()));
}

void NetworkPrepare::ReadChallengeAnswer(QObject* o){
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(o);
    Q_ASSERT(socket);

    bool ok = false;
    int answer_length =
        socket->property(AnswerLengthPropertyName).toInt(&ok);
    Q_ASSERT_X(ok, "NetworkPrepare::ReadChallengeAnswer",
                   "anser length property not an integer");

    if(socket->bytesAvailable() < answer_length)
        return;

    int node_id = socket->property(NodeIdPropertyName).toInt(&ok);
    Q_ASSERT_X(ok, "NetworkPrepare::ReadChallengeAnswer",
                   "node id property not an integer");

    QByteArray challenge =
        socket->property(ChallengePropertyName).toByteArray();
    Q_ASSERT(challenge.size() == ChallengeLength);

    QByteArray answer = socket->read(answer_length);
    if(!Crypto::GetInstance()->Verify(
                &_config->nodes[node_id].identity_pk,
                challenge,
                answer)){
        // XXX(scw): challenge failed message
        socket->disconnectFromHost();
        delete socket;
        return;
    }

    socket->setProperty(NodeIdPropertyName, QVariant());
    socket->setProperty(AnswerLengthPropertyName, QVariant());
    socket->setProperty(ChallengePropertyName, QVariant());
    disconnect(socket, SIGNAL(readyRead()),
               _answerSignalMapper, SLOT(map()));

    AddSocket(node_id, socket);
}

void NetworkPrepare::TryConnect(){
    connect(_connectSignalMapper, SIGNAL(mapped(int)),
            this, SLOT(Connected(int)));
    connect(_errorSignalMapper, SIGNAL(mapped(int)),
            this, SLOT(ConnectError(int)));

    foreach(const NodeInfo& node, _config->nodes){
        if(node.node_id >= _config->my_node_id)
            continue;

        QTcpSocket* socket = new QTcpSocket(_server);
        connect(socket, SIGNAL(connected()),
                _connectSignalMapper, SLOT(map()));
        connect(socket, SIGNAL(error(QAbstractSocket::SocketError)),
                _errorSignalMapper, SLOT(map()));
        _connectSignalMapper->setMapping(socket, socket);
        _errorSignalMapper->setMapping(socket, socket);
        socket->setProperty(NodeIdPropertyName, node.node_id);
        socket->connectToHost(node.addr, node.port);
    }
}

void NetworkPrepare::Connected(QObject* o){
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(o);
    Q_ASSERT(socket);

    disconnect(socket, SIGNAL(connected()),
               _connectSignalMapper, SLOT(map()));
    disconnect(socket, SIGNAL(error(QAbstractSocket::SocketError)),
               _errorSignalMapper, SLOT(map()));

    _challengeSignalMapper->setMapping(socket, socket);
    connect(socket, SIGNAL(readyRead()),
            _challengeSignalMapper, SLOT(map()));
}

void NetworkPrepare::ConnectError(QObject* o){
    // XXX(scw): error message? retry count? wait before retry?
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(o);
    Q_ASSERT(socket);

    bool ok = false;
    int node_id = socket->property(NodeIdPropertyName).toInt(&ok);
    Q_ASSERT_X(ok, "NetworkPrepare::ConnectError",
                   "node id property not an integer");

    const NodeInfo& node = _config->nodes[node_id];
    socket->connectToHost(node.addr, node.port);
}

void NetworkPrepare::ReadChallenge(QObject* o){
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(o);
    Q_ASSERT(socket);

    if(socket->bytesAvailable() < ChallengeLength)
        return;

    QByteArray challenge = socket->read(ChallengeLength);
    QByteArray answer;
    bool r = Crypto::GetInstance()->Sign(
            &_config->identity_sk, challenge, &answer);
    Q_ASSERT_X(r, "NetworkPrepare::ReadChallenge",
                  "challeng signing failed");
    QByteArrayUtil::PrependInt(answer.size(), &answer);
    QByteArrayUtil::PrependInt(_config->my_node_id, &answer);
    socket->write(answer);

    bool ok = false;
    int node_id = socket->property(NodeIdPropertyName).toInt(&ok);
    Q_ASSERT_X(ok, "NetworkPrepare::ConnectError",
                   "node id property not an integer");

    socket->setProperty(NodeIdPropertyName, QVariant());
    AddSocket(node_id, socket);
}
}
// -*- vim:sw=4:expandtab:cindent:
