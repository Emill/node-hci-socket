#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <poll.h>

#include <napi.h>
#include <uv.h>

#define BTPROTO_HCI 1
#define HCI_MAX_DEV 16

#define HCIDEVUP _IOW('H', 201, int)
#define HCIDEVDOWN _IOW('H', 202, int)
#define HCIDEVRESET _IOW('H', 203, int)
#define HCIDEVRESTAT _IOW('H', 204, int)
#define HCIGETDEVLIST _IOR('H', 210, int)
#define HCIGETDEVINFO _IOR('H', 211, int)

#define HCI_CHANNEL_USER 1
#define HCI_MAX_FRAME_SIZE 1028

namespace {
	struct hci_dev_req {
		uint16_t dev_id;
		uint32_t dev_opt;
	};

	struct hci_dev_list_req {
		uint16_t  dev_num;
		struct hci_dev_req dev_req[0];
	};

	struct hci_dev_stats {
		uint32_t err_rx, err_tx, cmd_tx, evt_rx, acl_tx, acl_rx, sco_tx, sco_rx, byte_rx, byte_tx;
	};

	struct hci_dev_info {
		uint16_t dev_id;
		char name[8];
		uint8_t bdaddr[6];
		uint32_t flags;
		uint8_t type;
		uint8_t features[8];
		uint32_t pkt_type;
		uint32_t link_policy;
		uint32_t link_mode;
		uint16_t acl_mtu;
		uint16_t acl_pkts;
		uint16_t sco_mtu;
		uint16_t sco_pkts;
		struct hci_dev_stats stat;
	};

	struct sockaddr_hci {
		sa_family_t hci_family;
		uint16_t hci_dev;
		uint16_t hci_channel;
	};
	
	class HciSocket : public Napi::ObjectWrap<HciSocket> {
	public:
		static Napi::Object Init(Napi::Env env, Napi::Object exports);
		HciSocket(const Napi::CallbackInfo& info);
	private:
		Napi::Value Bind(const Napi::CallbackInfo& info);
		Napi::Value Write(const Napi::CallbackInfo& info);
		void Close(const Napi::CallbackInfo& info);
		
		static Napi::Value GetDevList(const Napi::CallbackInfo& info);
		static Napi::Value GetDevInfo(const Napi::CallbackInfo& info);
		static Napi::Value HciUpOrDown(const Napi::CallbackInfo& info);
		
		void Destroy();
		
		static void UvPollCb(uv_poll_t *handle, int status, int events);
		
		int sk;
		uv_poll_t *poll_handle;
		Napi::FunctionReference callback;
	};
	
	HciSocket::HciSocket(const Napi::CallbackInfo& info)
	: Napi::ObjectWrap<HciSocket>(info) {
		this->sk = -1;
	}
	
	Napi::Value HciSocket::Bind(const Napi::CallbackInfo& info) {
		Napi::Env env = info.Env();
		size_t nargs = info.Length();
		if (nargs < 1 || !info[0].IsNumber() || floorf(info[0].As<Napi::Number>().DoubleValue()) != info[0].As<Napi::Number>().DoubleValue()) {
			Napi::TypeError::New(env, "Wrong first arg type, must be integer").ThrowAsJavaScriptException();
			return Napi::Value();
		}
		double dev_num_double = info[0].As<Napi::Number>().DoubleValue();
		if (dev_num_double < 0 || dev_num_double >= 0xffff) {
			Napi::TypeError::New(env, "Wrong first arg type, must be integer between 0 and 0xfffe").ThrowAsJavaScriptException();
			return Napi::Value();
		}
		uint16_t dev_num = (uint16_t)dev_num_double;
		if (nargs < 2 || !info[1].IsFunction()) {
			Napi::TypeError::New(env, "Wrong first arg type, must be function").ThrowAsJavaScriptException();
			return Napi::Value();
		}
		
		if (this->sk != -1) {
			Napi::Error::New(env, "Socket already bound").ThrowAsJavaScriptException();
			return Napi::Value();
		}
		
		int sk = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
		if (sk < 0) {
			return Napi::Number::New(env, -errno);
		}
		
		struct sockaddr_hci addr;
		memset(&addr, 0, sizeof(addr));
		addr.hci_family = AF_BLUETOOTH;
		addr.hci_dev = dev_num;
		addr.hci_channel = HCI_CHANNEL_USER;
		
		if (bind(sk, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
			close(sk);
			return Napi::Number::New(env, -errno);
		}
		
		uv_loop_t *loop;
		napi_get_uv_event_loop(env, &loop);
		
		this->poll_handle = (uv_poll_t *)malloc(sizeof(uv_poll_t));
		int res = uv_poll_init(loop, this->poll_handle, sk);
		if (res != 0) {
			close(sk);
			return Napi::Number::New(env, res);
		}
		this->poll_handle->data = (void *)this;
		
		uv_poll_start(this->poll_handle, UV_READABLE | UV_DISCONNECT, UvPollCb);
		
		this->sk = sk;
		this->callback = Napi::Persistent(info[1].As<Napi::Function>());
		this->Ref(); // Avoid being garbage collected (we Unref when the socket is closed)
		
		return Napi::Number::New(env, 0);
	}
	
	Napi::Value HciSocket::Write(const Napi::CallbackInfo& info) {
		Napi::Env env = info.Env();
		if (this->sk == -1) {
			Napi::Error::New(env, "Socket is not open").ThrowAsJavaScriptException();
			return Napi::Value();
		}
		size_t nargs = info.Length();
		if (nargs < 1 || !info[0].IsBuffer()) {
			Napi::TypeError::New(env, "Argument must be a Buffer").ThrowAsJavaScriptException();
			return Napi::Value();
		}
		
		Napi::Buffer<uint8_t> buffer = info[0].As<Napi::Buffer<uint8_t>>();
		
		if (buffer.Length() < 4 || buffer.Length() > HCI_MAX_FRAME_SIZE) {
			Napi::Error::New(env, "Buffer length must be between 4 and 1028 bytes").ThrowAsJavaScriptException();
			return Napi::Value();
		}
		
		// The libuv sets the socket to non-blocking, but we would like blocking writes,
		// since it's extremely uncommon the write actually would block (I guess?).
		// Ignore POLLERR result since errors are caught at the actual write (and by libuv).
		struct pollfd p;
		p.fd = this->sk;
		p.events = POLLOUT;
		do {
			int pollres = poll(&p, 1, -1);
			if (pollres == -1 && errno == EINTR) {
				continue;
			}
		} while(0);
		
		ssize_t ret = write(this->sk, buffer.Data(), buffer.Length());
		if (ret == -1) {
			ret = -errno;
		}
		return Napi::Number::New(env, ret);
	}
	
	void HciSocket::Close(const Napi::CallbackInfo& info) {
		Destroy();
	}
	
	void HciSocket::Destroy() {
		if (this->sk != -1) {
			uv_poll_stop(this->poll_handle);
			uv_close((uv_handle_t *)this->poll_handle, (uv_close_cb)free);
			close(this->sk);
			this->sk = -1;
			
			this->callback.Call({});
			this->callback.Reset();
			this->Unref();
		}
	}
	
	void HciSocket::UvPollCb(uv_poll_t *handle, int status, int events) {
		HciSocket *me = (HciSocket *)handle->data;
		Napi::Env env = me->Env();
		
		Napi::HandleScope scope(env);
		//fprintf(stderr, "%p status %d events %d\n", me, status, events);
		
		// If the status is nonzero, on Linux it always corresponds to -EBADBF which
		// is manually set by libuv when POLLERR && !POLLPRI. In any case, just read
		// the socket to get the real error.
		
		uint8_t packet[HCI_MAX_FRAME_SIZE];
		ssize_t nbytes = read(me->sk, packet, HCI_MAX_FRAME_SIZE);
		if (nbytes <= 0) {
			me->Destroy();
		} else {
			Napi::Buffer<uint8_t> buf = Napi::Buffer<uint8_t>::Copy(env, packet, nbytes);
			me->callback.Call({buf});
		}
		if (env.IsExceptionPending()) {
			napi_fatal_exception(env, env.GetAndClearPendingException().Value());
		}
	}
	
	void StoreDevInfo(Napi::Object& obj, struct hci_dev_info& di) {
		char bdaddr[18];
		sprintf(bdaddr, "%02X:%02X:%02X:%02X:%02X:%02X", di.bdaddr[5], di.bdaddr[4], di.bdaddr[3], di.bdaddr[2], di.bdaddr[1], di.bdaddr[0]);
		size_t dev_type = (di.type >> 4) & 0x03;
		size_t bus_type = di.type & 0x0f;
		
		static const char *dev_types[] = {
			"PRIMARY",
			"AMP"
		};
		
		static const char *bus_types[] = {
			"VIRTUAL",
			"USB",
			"PCCARD",
			"UART",
			"RS232",
			"PCI",
			"SDIO",
			"SPI",
			"I2C",
			"SMD",
			"VIRTIO"
		};
		
		obj.Set("devId", di.dev_id);
		obj.Set("name", di.name);
		obj.Set("bdaddr", bdaddr);
		obj.Set("flags", di.flags);
		
		if (dev_type < sizeof(dev_types) / sizeof(dev_types[0])) {
			obj.Set("type", dev_types[dev_type]);
		} else {
			obj.Set("type", dev_type);
		}
		if (bus_type < sizeof(bus_types) / sizeof(bus_types[0])) {
			obj.Set("bus", bus_types[bus_type]);
		} else {
			obj.Set("bus", bus_type);
		}
	}
	
	Napi::Value HciSocket::GetDevList(const Napi::CallbackInfo& info) {
		Napi::Env env = info.Env();
		int sk = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
		if (sk < 0) {
			return Napi::Number::New(env, -errno);
		}
		
		struct hci_dev_list_req* dl;
		dl = (struct hci_dev_list_req*)malloc(sizeof(*dl) + HCI_MAX_DEV * sizeof(struct hci_dev_req));
		dl->dev_num = HCI_MAX_DEV;
		if (ioctl(sk, HCIGETDEVLIST, (void*)dl) == -1) {
			free(dl);
			close(sk);
			return Napi::Number::New(env, -errno);
		}
		
		Napi::Array a = Napi::Array::New(env);
		
		int nfound = 0;
		for (int i = 0; i < dl->dev_num; i++) {
			struct hci_dev_info di;
			di.dev_id = dl->dev_req[i].dev_id;
			if (ioctl(sk, HCIGETDEVINFO, (void*)&di) != -1) {
				Napi::Object obj = Napi::Object::New(env);
				StoreDevInfo(obj, di);
				a.Set(nfound++, obj);
			}
		}
		
		free(dl);
		close(sk);
		
		return a;
	}
	
	Napi::Value HciSocket::GetDevInfo(const Napi::CallbackInfo& info) {
		Napi::Env env = info.Env();
		int sk = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
		if (sk < 0) {
			return Napi::Number::New(env, -errno);
		}
		
		uint16_t dev_id = 0;
		if (info.Length() >= 1 && info[0].IsNumber()) {
			dev_id = (uint16_t)info[0].As<Napi::Number>().Uint32Value();
		}
		
		Napi::Value ret;
		
		struct hci_dev_info di;
		di.dev_id = dev_id;
		if (ioctl(sk, HCIGETDEVINFO, (void *)&di) != -1) {
			Napi::Object obj = Napi::Object::New(env);
			StoreDevInfo(obj, di);
			ret = obj;
		} else {
			ret = Napi::Number::New(env, -errno);
		}
		
		close(sk);
		return ret;
	}
	
	Napi::Value HciSocket::HciUpOrDown(const Napi::CallbackInfo& info) {
		Napi::Env env = info.Env();
		int sk = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
		if (sk < 0) {
			return Napi::Number::New(env, -errno);
		}
		
		uint16_t dev_id = 0;
		if (info.Length() >= 1 && info[0].IsNumber()) {
			dev_id = (uint16_t)info[0].As<Napi::Number>().Uint32Value();
		}
		
		bool up = info.Length() >= 2 && info[1].IsBoolean() && info[1].As<Napi::Boolean>().Value();
		int ret = 0;
		
		if (ioctl(sk, up ? HCIDEVUP : HCIDEVDOWN, dev_id) == -1) {
			ret = -errno;
		}
		
		close(sk);
		
		return Napi::Number::New(env, ret);
	}
	
	Napi::Object HciSocket::Init(Napi::Env env, Napi::Object exports) {
		Napi::Function func =
			DefineClass(env,
						"HciSocket",
						{InstanceMethod("bind", &HciSocket::Bind),
						InstanceMethod("write", &HciSocket::Write),
						InstanceMethod("close", &HciSocket::Close),
						StaticMethod("getDevList", &HciSocket::GetDevList),
						StaticMethod("getDevInfo", &HciSocket::GetDevInfo),
						StaticMethod("hciUpOrDown", &HciSocket::HciUpOrDown)});
		
		exports.Set("HciSocket", func);
		return exports;
	}
	
	Napi::Object Init(Napi::Env env, Napi::Object exports) {
		return HciSocket::Init(env, exports);
	}
	
	NODE_API_MODULE(NODE_GYP_MODULE_NAME, Init);
}
