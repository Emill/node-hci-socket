{
	"targets": [
		{
			"target_name": "hci_socket_native_support",
			"cflags!": [ "-fno-exceptions" ],
			"cflags_cc!": [ "-fno-exceptions" ],
			"sources": ["src/hci-socket.cpp"],
			"include_dirs": [
				"<!@(node -p \"require('node-addon-api').include\")"
			],
			"defines": [ 'NAPI_DISABLE_CPP_EXCEPTIONS' ],
		}
	]
}
