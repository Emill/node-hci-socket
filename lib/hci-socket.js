const EventEmitter = require('events');
const util = require('util');
const constants = require('constants');
const native = require('bindings')('hci_socket_native_support.node');

const HCI_UP = 0;

const errnos = {
	EPERM: 1,
	ENOENT: 2,
	ESRCH: 3,
	EINTR: 4,
	EIO: 5,
	ENXIO: 6,
	E2BIG: 7,
	ENOEXEC: 8,
	EBADF: 9,
	ECHILD: 10,
	EAGAIN: 11,
	ENOMEM: 12,
	EACCES: 13,
	EFAULT: 14,
	ENOTBLK: 15,
	EBUSY: 16,
	EEXIST: 17,
	EXDEV: 18,
	ENODEV: 19,
	ENOTDIR: 20,
	EISDIR: 21,
	EINVAL: 22,
	ENFILE: 23,
	EMFILE: 24,
	ENOTTY: 25,
	ETXTBSY: 26,
	EFBIG: 27,
	ENOSPC: 28,
	ESPIPE: 29,
	EROFS: 30,
	EMLINK: 31,
	EPIPE: 32,
	EDOM: 33,
	ERANGE: 34
};

const errnosInv = (function() {
	var inv = Object.create(null);
	for (var name in errnos) {
		if (errnos.hasOwnProperty(name)) {
			inv[errnos[name]] = name;
		}
	}
	return inv;
})();

function throwError(msg, errnoNeg) {
	var err = new Error(msg + ': ' + errnosInv[-errnoNeg]);
	err.errno = errnoNeg;
	err.code = errnosInv[-errnoNeg];
	throw err;
}

function getDevList() {
	var res = native.HciSocket.getDevList();
	if (Number.isInteger(res)) {
		throwError('Could not get dev list', res);
	}
	return res;
}

function getDevInfo(devId) {
	var res = native.HciSocket.getDevInfo(devId);
	if (Number.isInteger(res)) {
		throwError('Could not get dev info for hci' + devId, res);
	}
	return res;
}

function HciSocket(devId) {
	EventEmitter.call(this);
	var di;
	if (typeof devId !== 'number') {
		devId = 0;
		var list = getDevList();
		if (!Number.isInteger(list) && list[0]) {
			di = list[0];
			devId = di.devId;
		}
	}
	
	if (!di) {
		di = native.HciSocket.getDevInfo(devId);
		if (Number.isInteger(di)) {
			throwError('hci' + devId + ' not found', di);
		}
	}
	if (di.flags & (1 << HCI_UP)) {
		var res = native.HciSocket.hciUpOrDown(devId, false); // Bring down interface
		if (res < 0) {
			throwError('Could not bring down hci' + devId + ' interface', res);
		}
	}
	var socket = new native.HciSocket();
	var res = socket.bind(devId, data => {
		if (data instanceof Buffer) {
			this.emit('data', data);
		} else {
			this.emit('close');
		}
	});
	if (res < 0) {
		throwError('Could not bind socket hci' + devId, res);
	}
	
	this.write = function write(data) {
		// If error is returned, then the socket should be be closed soon automatically
		return socket.write(data);
	};
	this.close = function close() {
		socket.close();
	};
}
util.inherits(HciSocket, EventEmitter);

HciSocket.getDevList = getDevList;
HciSocket.getDevInfo = getDevInfo;

module.exports = HciSocket;
