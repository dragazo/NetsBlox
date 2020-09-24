'use strict';
const getRPCLogger = require('../utils/logger');
const acl = require('../roboscape/accessControl');
const HEALTHYPI_MODE = process.env.HEALTHYPI_MODE || 'both';
const ciphers = require('../roboscape/ciphers');

// these might be better defined as an attribute on the sensor
const FORGET_TIME = 120; // forgetting a sensor in seconds
const RESPONSE_TIMEOUT = 200; // waiting for response in milliseconds

var Sensor = function (mac_addr, ip4_addr, ip4_port, aServer) {
    this.id = mac_addr;
    this.mac_addr = mac_addr;
    this.ip4_addr = ip4_addr;
    this.ip4_port = ip4_port;
    this.heartbeats = 0;
    this.timestamp = -1; // time of last message in sensor time
    this.sockets = []; // uuids of sockets of registered clients
    this.callbacks = {}; // callbacks keyed by msgType
    this.encryptionKey = [0]; // encryption key
    this.encryptionMethod = ciphers.caesar; // backward compat
    this.buttonDownTime = 0; // last time button was pressed
    // rate control
    this.totalCount = 0; // in messages per second
    this.totalRate = 0; // in messages per second
    this.clientRate = 0; // in messages per second
    this.clientPenalty = 0; // in seconds
    this.clientCounts = {};
    this.lastSeqNum = -1; // initially disabled
    this._logger = getRPCLogger(`healthypi:${mac_addr}`);
    this.server = aServer; // a handle to the udp server for communication with the sensor
};

Sensor.prototype.setTotalRate = function (rate) {
    this._logger.log('set total rate ' + this.mac_addr + ' ' + rate);
    this.totalRate = Math.max(rate, 0);
};

Sensor.prototype.setClientRate = function (rate, penalty) {
    this._logger.log('set client rate ' + this.mac_addr + ' ' + rate + ' ' + penalty);
    this.clientRate = Math.max(rate, 0);
    this.clientPenalty = Math.min(Math.max(penalty, 0), 60);
};

Sensor.prototype.resetRates = function () {
    this._logger.log('reset rate limits');
    this.totalRate = 0;
    this.clientRate = 0;
    this.clientPenalty = 0;
    this.clientCounts = {};
};

// resets the encryption
// for backward compat sets it to caesar cipher with key [0]
Sensor.prototype.resetEncryption = function () {
    this._logger.log('resetting encryption');
    // null would make more sense but keeping backward compatibility here
    this.encryptionMethod = ciphers.caesar;
    this.encryptionKey = [0];
};

Sensor.prototype.resetSeqNum = function () {
    this._logger.log('resetting seq numbering');
    this.setSeqNum(-1);
};


Sensor.prototype.updateAddress = function (ip4_addr, ip4_port) {
    this.ip4_addr = ip4_addr;
    this.ip4_port = ip4_port;
    this.heartbeats = 0;
};

Sensor.prototype.setSeqNum = function (seqNum) {
    this.lastSeqNum = seqNum;
};

Sensor.prototype.accepts = function (clientId, seqNum) {
    if (this.lastSeqNum >= 0 && (seqNum <= this.lastSeqNum ||
            seqNum > this.lastSeqNum + 100)) {
        return false;
    }

    var client = this.clientCounts[clientId];
    if (!client) {
        client = {
            count: 0,
            penalty: 0
        };
        this.clientCounts[clientId] = client;
    }

    if (client.penalty > 0) {
        this._logger.log(clientId + ' client penalty');
        return false;
    }

    if (this.clientRate !== 0 && client.count + 1 > this.clientRate) {
        this._logger.log(clientId + ' client rate violation');
        client.penalty = 1 + this.clientPenalty;
        return false;
    }

    if (this.totalRate !== 0 && this.totalCount + 1 > this.totalRate) {
        this._logger.log(clientId + ' total rate violation');
        return false;
    }

    this.totalCount += 1;
    client.count += 1;
    return true;
};

Sensor.prototype.heartbeat = function () {
    this.totalCount = 0;
    for (var id in this.clientCounts) {
        var client = this.clientCounts[id];
        client.count = 0;
        if (client.penalty > 1) {
            client.count = 0;
            client.penalty -= 1;
        } else {
            delete this.clientCounts[id];
        }
    }

    this.heartbeats += 1;
    if (this.heartbeats >= FORGET_TIME) {
        return false;
    }
    return true;
};

Sensor.prototype.isAlive = function () {
    return this.heartbeats <= 2;
};

Sensor.prototype.isMostlyAlive = function () {
    return this.heartbeats <= FORGET_TIME;
};

Sensor.prototype.addClientSocket = function (socket) {
    const {clientId} = socket;
    var i = this.sockets.findIndex(s => s.clientId === clientId);
    if (i < 0) {
        this._logger.log('register ' + clientId + ' ' + this.mac_addr);
        this.sockets.push(socket);
        return true;
    }
    return false;
};

Sensor.prototype.removeClientSocket = function (socket) {
    const {clientId} = socket;
    var i = this.sockets.findIndex(s => s.clientId === clientId);
    if (i >= 0) {
        this._logger.log('unregister ' + clientId + ' ' + this.mac_addr);
        this.sockets.splice(i, 1);
        return true;
    }
    return false;
};

Sensor.prototype.sendToSensor = function (message) {
    this.server.send(message, this.ip4_port, this.ip4_addr, function (err) {
        if (err) {
            this._logger.log('send error ' + err);
        }
    });
};

Sensor.prototype.receiveFromSensor = function (msgType, timeout) {
    if (!this.callbacks[msgType]) {
        this.callbacks[msgType] = [];
    }
    var callbacks = this.callbacks[msgType];

    return new Promise(function (resolve) {
        callbacks.push(resolve);
        setTimeout(function () {
            var i = callbacks.indexOf(resolve);
            if (i >= 0) {
                callbacks.splice(i, 1);
            }
            resolve(false);
        }, timeout || RESPONSE_TIMEOUT);
    });
};

Sensor.prototype.getVitals = function () {
    this._logger.log('get vitals ' + this.mac_addr);
    let promise = this.receiveFromSensor('vitals');
    let message = Buffer.alloc(1);
    message.write('V', 0, 1);
    this.sendToSensor(message);
    return promise.then(value => {
        console.log(`vitals response: ${value}`);
        return value;   
    });
};

Sensor.prototype.commandToClient = function (command) {
    if (HEALTHYPI_MODE === 'security' || HEALTHYPI_MODE === 'both') {
        var mac_addr = this.mac_addr;
        this.sockets.forEach(socket => {
            const content = {
                sensor: mac_addr,
                command: command
            };
            socket.sendMessage('sensor command', content);
        });
    }
};

Sensor.prototype.sendToClient = function (msgType, content) {
    var myself = this;

    let fields = ['time', ...Object.keys(content)];
    content.sensor = this.mac_addr;
    content.time = this.timestamp;

    if (msgType !== 'set led') {
        this._logger.log('event ' + msgType + ' ' + JSON.stringify(content));
    }

    if (this.callbacks[msgType]) {
        var callbacks = this.callbacks[msgType];
        delete this.callbacks[msgType];
        callbacks.forEach(function (callback) {
            callback(content);
        });
        callbacks.length = 0;
    }

    this.sockets.forEach(async socket => {
        await acl.ensureAuthorized(socket.username, myself.mac_addr); // should use sensorId instead of mac_addr

        if (HEALTHYPI_MODE === 'native' || HEALTHYPI_MODE === 'both') {
            socket.sendMessage(msgType, content);
        }

        if ((HEALTHYPI_MODE === 'security' && msgType !== 'set led') ||
            HEALTHYPI_MODE === 'both') {
            var text = msgType;
            for (var i = 0; i < fields.length; i++) {
                text += ' ' + content[fields[i]];
            }

            const encryptedContent = {
                sensor: myself.mac_addr,
                message: this._hasValidEncryptionSet() ? myself.encrypt(text.trim()) : text.trim()
            };
            socket.sendMessage('sensor message', encryptedContent);
        }
    });
};

// used for handling incoming message from the sensor
Sensor.prototype.onMessage = function (message) {
    if (message.length < 11) {
        this._logger.log('invalid message ' + this.ip4_addr + ':' + this.ip4_port +
            ' ' + message.toString('hex'));
        return;
    }

    var oldTimestamp = this.timestamp;
    this.timestamp = message.readUInt32LE(6);
    var command = message.toString('ascii', 10, 11);
    var state;

    if (command === 'I' && message.length === 11) {
        if (this.timestamp < oldTimestamp) {
            this._logger.log('sensor was rebooted ' + this.mac_addr);
            this.resetSeqNum();
            this.setEncryptionKey([]);
            this.resetRates();
        }
    } else if (command === 'W' && message.length === 12) {
        state = message.readUInt8(11);
        this.sendToClient('whiskers', {
            left: (state & 0x2) == 0,
            right: (state & 0x1) == 0
        });
    } else if (command === 'P' && message.length === 12) {
        state = message.readUInt8(11) == 0;
        if (HEALTHYPI_MODE === 'native' || HEALTHYPI_MODE === 'both') {
            this.sendToClient('button', {
                pressed: state
            });
        }
        if (HEALTHYPI_MODE === 'security' || HEALTHYPI_MODE === 'both') {
            if (state) {
                this.buttonDownTime = new Date().getTime();
                setTimeout(function (sensor, pressed) {
                    if (sensor.buttonDownTime === pressed) {
                        sensor.resetSensor();
                    }
                }, 1000, this, this.buttonDownTime);
            } else {
                if (new Date().getTime() - this.buttonDownTime < 1000) {
                    this.randomEncryption();
                }
                this.buttonDownTime = 0;
            }
        }
    } else if (command === 'V' && message.length === 16) {
        this.sendToClient('vitals', {
            heartRate: message.readUInt8(11),
            respirationRate: message.readUInt8(12),
            spo2: message.readUInt8(13),
            temperature: message.readInt16LE(14),
        });
    } else {
        this._logger.log('unknown ' + this.ip4_addr + ':' + this.ip4_port +
            ' ' + message.toString('hex'));
    }
};

// handle user commands to the sensor (through the 'send' rpc)
Sensor.prototype.onCommand = function(command) {
    const cases = [
        {
            regex: /^is alive$/,
            handler: () => {
                this.sendToClient('alive', {});
                return this.isAlive();
            }
        },
        {
            regex: /^get vitals$/,
            handler: () => {
                return this.getVitals();
            }
        },
        {
            regex: /^set encryption ([^ ]+)(| -?\d+([ ,]-?\d+)*)$/, // name of the cipher
            handler: () => {
                let cipherName = RegExp.$1.toLowerCase();
                var key = RegExp.$2.split(/[, ]/);
                if (key[0] === '') {
                    key.splice(0, 1);
                }
                return this.setEncryptionMethod(cipherName) && this.setEncryptionKey(key);
            }
        },
        { // deprecated
            regex: /^set key(| -?\d+([ ,]-?\d+)*)$/,
            handler: () => {
                var encryption = RegExp.$1.split(/[, ]/);
                if (encryption[0] === '') {
                    encryption.splice(0, 1);
                }
                return this.setEncryptionKey(encryption.map(Number));
            }
        },
        {
            regex: /^set total rate (-?\d+)$/,
            handler: () => {
                this.setTotalRate(+RegExp.$1);
            }
        },
        {
            regex: /^set client rate (-?\d+)[, ](-?\d+)$/,
            handler: () => {
                this.setClientRate(+RegExp.$1, +RegExp.$2);
            }
        },
        {
            regex: /^reset seq$/,
            handler: () => {
                this.resetSeqNum();
            }
        },
        {
            regex: /^reset rates$/,
            handler: () => {
                this.resetRates();
            }
        },
    ];

    let matchingCase = cases.find(aCase => command.match(aCase.regex));
    if (!matchingCase) return false; // invalid command structure

    let rv = matchingCase.handler();
    if (rv === undefined) rv = true;
    return rv;
};

// determines whether encryption/decryption can be activated or not
Sensor.prototype._hasValidEncryptionSet = function () {
    let verdict = (this.encryptionKey && this.encryptionMethod && Array.isArray(this.encryptionKey) && this.encryptionKey.length !== 0);
    return verdict;
};

Sensor.prototype.encrypt = function (text) {
    if (!this._hasValidEncryptionSet()) {
        throw new Error('invalid encryption setup');
    }
    let output = this.encryptionMethod.encrypt(text, this.encryptionKey);
    this._logger.log('"' + text + '" encrypted to "' + output + '"');
    return output;
};

Sensor.prototype.decrypt = function (text) {
    if (!this._hasValidEncryptionSet()) {
        throw new Error('invalid encryption setup');
    }
    let output = this.encryptionMethod.decrypt(text, this.encryptionKey);
    this._logger.log('"' + text + '" decrypted to "' + output + '"');
    return output;
};

// disable encryption and decryption with minimal changes
Sensor.prototype.disableEncryption = function () {
    this.encryptionMethod = ciphers.plain;
};

Sensor.prototype.setEncryptionMethod = function (name) {
    if (!ciphers[name]) {
        this._logger.warn('invalid cipher name ' + name);
        return false;
    }
    this._logger.log('setting cipher to ' + name);

    this.encryptionMethod = ciphers[name];
    return true;
};

// WARN keys number?
Sensor.prototype.setEncryptionKey = function (keys) {
    if (!this.encryptionMethod) {
        this._logger.warn('setting the key without a cipher ' + keys);
        return false;
    } else if (keys instanceof Array) { // all the supported ciphers require an array of numbers
        keys = keys.map(num => parseInt(num));
        this.encryptionKey = keys;
        this._logger.log(this.mac_addr, 'encryption key set to', keys);
        return true;
    } else {
        this._logger.warn('invalid encryption key ' + keys);
        return false;
    }
};

Sensor.prototype.randomEncryption = function () {
    var keys = [],
        blinks = [];
    for (var i = 0; i < 4; i++) {
        var a = Math.floor(Math.random() * 16);
        keys.push(a);
        blinks.push(a & 0x8 ? 2 : 1);
        blinks.push(a & 0x4 ? 2 : 1);
        blinks.push(a & 0x2 ? 2 : 1);
        blinks.push(a & 0x1 ? 2 : 1);
    }
    blinks.push(3);
    this.resetSeqNum();
    this.resetRates();
    this.setEncryptionKey(keys);
    this.playBlinks(blinks);
};

// resets encryption, sequencing, and rate limits
Sensor.prototype.resetSensor = function () {
    this._logger.log('resetting sensor');
    this.resetSeqNum();
    this.resetRates();
    this.resetEncryption();
    this.playBlinks([3]);
};

module.exports = Sensor;
