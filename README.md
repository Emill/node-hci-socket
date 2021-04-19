# node-hci-socket

Linux bindings for using a Bluetooth controller in Node.js over HCI.

This module uses the `HCI_CHANNEL_USER` feature in the Linux kernel, allowing a process to communicate over HCI directly with a Bluetooth controller.

## Setup

Installation:

```
npm install hci-socket
```

## Usage

Include the `HciSocket` class:

```javascript
const HciSocket = require('hci-socket');
```

List devices:

```javascript
> HciSocket.getDevList();

[
  {
    devId: 0,
    name: 'hci0',
    bdaddr: 'XX:XX:XX:XX:XX:XX',
    flags: 0,
    type: 'PRIMARY',
    bus: 'USB'
  }
]
```

Get info about a specific `devId`:

```javascript
> HciSocket.getDevInfo(0);

{
  devId: 0,
  name: 'hci0',
  bdaddr: 'XX:XX:XX:XX:XX:XX',
  flags: 0,
  type: 'PRIMARY',
  bus: 'USB'
}
```

An `Error` with `code` `'ENODEV'` will be thrown if not found.

Create a `HciSocket` instance:

```javascript
var socket = new HciSocket(); // To create a socket for the first found hci device
// OR
var socket = new HciSocket(devId);
```

* An `Error` with `code` `'ENODEV'` will be thrown if not found.
* An `Error` with `code` `'EBUSY'` will be thrown if the device is already in use. You could try to stop `bluetoothd` if it is running.
* An `Error` with `code` `'EPERM'` will be thrown if the node process does not have permissions to use the socket. Execute ``sudo setcap cap_net_admin=ep $(eval readlink -f `which node`)`` first or run node using sudo.

The `HciSocket` instance has two methods.

Write a packet:

```javascript
socket.write(Buffer.from([0x01, 0x03, 0x0C, 0x00])); // sends a Reset command
```

The packet format used is the same as specified in the Bluetooth Core specification, Vol 4: Host Controller Interface, Part A: UART Transport Layer, Chapter 2: Protocol. Note that even if for example a USB Bluetooth controller is used, the Linux kernel will automatically convert it into this format.

An exception will be thrown if the socket has been closed, or if the argument is not a `Buffer` with length between 4 and 1028.

Close a socket:

```javascript
socket.close();
```

The `HciSocket` instance has two events defined.

A packet arrives:

```javascript
socket.on('data', function(buffer) { ... });
```

The socket gets closed, either explicitly after calling `close()`, or automatically if the device gets unplugged from the system:

```javascript
socket.on('close', function() { ... });
```

A `HciSocket` instance that has not been closed will keep the node process from exiting.
