// xlite — туннель VLESS для mincord, вкомпонованный прямо в программу (go build -buildmode=c-archive):
// вход SOCKS5 на 127.0.0.1 (TCP+UDP, логин/пароль) и выход VLESS поверх Reality или TLS, транспорт
// TCP или WebSocket. Весь сетевой и криптографический код — официальный Xray-core v26.10.31; здесь
// только сборка конфигурации без модуля разбора JSON (он тянет все протоколы и раздувает программу).
// Правила разбора повторяют infra/conf (REALITYConfig, VLessOutboundConfig, SocksServerConfig).
//
// Для C (mincord, src/vless.c):
//   int  XliteStart(const char *cfg, int n);  0 — работает; 1 — неверные параметры; 2 — Xray не
//                                             создал экземпляр; 3 — не запустился (порт занят и т.п.)
//   void XliteStop(void);                     остановить и вернуть память системе
// cfg — строки "ключ=значение":
//   port, user, pass                  — локальный SOCKS5
//   uuid, host, rport, flow, enc      — сервер VLESS
//   security=reality|tls, sni, fp      — общее
//   pbk, sid, spx, pqv                — Reality (pqv — постквантовая проверка подписи сервера)
//   alpn                              — TLS (через запятую)
//   type=tcp|ws, wshost, wspath       — транспорт
// Журналы Xray выключены; на stdout/stderr ничего не пишется.
package main

import "C"

import (
	"encoding/base64"
	"encoding/hex"
	"errors"
	"net/url"
	"runtime"
	"runtime/debug"
	"strconv"
	"strings"
	"sync"
	"unsafe"

	"github.com/xtls/xray-core/app/dispatcher"
	applog "github.com/xtls/xray-core/app/log"
	"github.com/xtls/xray-core/app/policy"
	"github.com/xtls/xray-core/app/proxyman"
	_ "github.com/xtls/xray-core/app/proxyman/inbound"
	_ "github.com/xtls/xray-core/app/proxyman/outbound"
	clog "github.com/xtls/xray-core/common/log"
	"github.com/xtls/xray-core/common/net"
	"github.com/xtls/xray-core/common/protocol"
	"github.com/xtls/xray-core/common/serial"
	"github.com/xtls/xray-core/common/uuid"
	"github.com/xtls/xray-core/core"
	"github.com/xtls/xray-core/proxy/socks"
	"github.com/xtls/xray-core/proxy/vless"
	vout "github.com/xtls/xray-core/proxy/vless/outbound"
	"github.com/xtls/xray-core/transport/internet"
	"github.com/xtls/xray-core/transport/internet/reality"
	_ "github.com/xtls/xray-core/transport/internet/tcp"
	"github.com/xtls/xray-core/transport/internet/tls"
	"github.com/xtls/xray-core/transport/internet/websocket"
)

var (
	mu   sync.Mutex
	inst *core.Instance
	bad  = errors.New("bad")
)

func init() {
	// Среда Go внутри mincord — на минимум ресурсов: один рабочий поток, чаще сборка мусора,
	// мягкий потолок кучи.
	runtime.GOMAXPROCS(1)
	debug.SetGCPercent(50)
	debug.SetMemoryLimit(12 << 20)
}

func parseKV(text string) (map[string]string, error) {
	kv := map[string]string{}
	for _, line := range strings.Split(text, "\n") {
		if line == "" {
			continue
		}
		i := strings.IndexByte(line, '=')
		if i <= 0 {
			return nil, bad
		}
		kv[line[:i]] = line[i+1:]
	}
	return kv, nil
}

func build(kv map[string]string) (*core.Config, error) {
	port, err := strconv.Atoi(kv["port"])
	if err != nil || port <= 0 || port > 65535 {
		return nil, bad
	}
	rport, err := strconv.Atoi(kv["rport"])
	if err != nil || rport <= 0 || rport > 65535 {
		return nil, bad
	}
	id, err := uuid.ParseString(kv["uuid"])
	if err != nil {
		return nil, bad
	}
	flow := kv["flow"]
	switch flow {
	case "", vless.XRV, vless.XRV + "-udp443":
	default:
		return nil, bad
	}
	enc := kv["enc"]
	if enc == "" {
		enc = "none"
	}
	if enc != "none" {
		return nil, bad // постквантовое шифрование VLESS (mlkem768x25519plus) этой сборкой не поддерживается
	}

	// --- транспорт и его защита ---
	stream := &internet.StreamConfig{}
	switch kv["type"] {
	case "", "tcp":
		stream.ProtocolName = "tcp"
	case "ws":
		stream.ProtocolName = "websocket"
		ws := &websocket.Config{Host: kv["wshost"], Path: kv["wspath"]}
		if ws.Path == "" {
			ws.Path = "/"
		}
		stream.TransportSettings = []*internet.TransportConfig{{ProtocolName: "websocket", Settings: serial.ToTypedMessage(ws)}}
	default:
		return nil, bad
	}
	fp := strings.ToLower(kv["fp"])
	if fp == "" {
		fp = "chrome"
	}
	if fp == "unsafe" || fp == "hellogolang" || tls.GetFingerprint(fp) == nil {
		return nil, bad
	}
	switch kv["security"] {
	case "reality":
		r := &reality.Config{Fingerprint: fp, ServerName: kv["sni"]}
		if r.PublicKey, err = base64.RawURLEncoding.DecodeString(kv["pbk"]); err != nil || len(r.PublicKey) != 32 {
			return nil, bad
		}
		sid := kv["sid"]
		if len(sid) > 16 {
			return nil, bad
		}
		r.ShortId = make([]byte, 8)
		if _, err = hex.Decode(r.ShortId, []byte(sid)); err != nil {
			return nil, bad
		}
		if pqv := kv["pqv"]; pqv != "" {
			if r.Mldsa65Verify, err = base64.RawURLEncoding.DecodeString(pqv); err != nil || len(r.Mldsa65Verify) != 1952 {
				return nil, bad
			}
		}
		spx := kv["spx"]
		if spx == "" {
			spx = "/"
		}
		if spx[0] != '/' {
			return nil, bad
		}
		r.SpiderY = make([]int64, 10)
		u, perr := url.Parse(spx)
		if perr != nil {
			return nil, bad
		}
		q := u.Query()
		parse := func(param string, index int) {
			if q.Get(param) != "" {
				s := strings.Split(q.Get(param), "-")
				r.SpiderY[index], _ = strconv.ParseInt(s[0], 10, 64)
				if len(s) == 1 {
					r.SpiderY[index+1], _ = strconv.ParseInt(s[0], 10, 64)
				} else {
					r.SpiderY[index+1], _ = strconv.ParseInt(s[1], 10, 64)
				}
			}
			q.Del(param)
		}
		parse("p", 0)
		parse("c", 2)
		parse("t", 4)
		parse("i", 6)
		parse("r", 8)
		u.RawQuery = q.Encode()
		r.SpiderX = u.String()
		stream.SecurityType = serial.GetMessageType(r)
		stream.SecuritySettings = []*serial.TypedMessage{serial.ToTypedMessage(r)}
	case "tls":
		t := &tls.Config{ServerName: kv["sni"], Fingerprint: fp}
		if a := kv["alpn"]; a != "" {
			t.NextProtocol = strings.Split(a, ",")
		}
		stream.SecurityType = serial.GetMessageType(t)
		stream.SecuritySettings = []*serial.TypedMessage{serial.ToTypedMessage(t)}
	default:
		return nil, bad // без TLS VLESS — открытый текст
	}

	// --- выход VLESS ---
	account := &vless.Account{Id: id.String(), Flow: flow, Encryption: enc}
	server := &protocol.ServerEndpoint{
		Address: net.NewIPOrDomain(net.ParseAddress(kv["host"])),
		Port:    uint32(rport),
		User:    &protocol.User{Account: serial.ToTypedMessage(account)},
	}
	outbound := &core.OutboundHandlerConfig{
		Tag:            "out",
		SenderSettings: serial.ToTypedMessage(&proxyman.SenderConfig{StreamSettings: stream}),
		ProxySettings:  serial.ToTypedMessage(&vout.Config{Vnext: server}),
	}

	// --- вход SOCKS5 на 127.0.0.1 ---
	user, pass := kv["user"], kv["pass"]
	if user == "" || pass == "" {
		return nil, bad
	}
	lo := net.NewIPOrDomain(net.LocalHostIP)
	inbound := &core.InboundHandlerConfig{
		Tag: "in",
		ReceiverSettings: serial.ToTypedMessage(&proxyman.ReceiverConfig{
			PortList: &net.PortList{Range: []*net.PortRange{net.SinglePortRange(net.Port(port))}},
			Listen:   lo,
		}),
		ProxySettings: serial.ToTypedMessage(&socks.ServerConfig{
			AuthType:   socks.AuthType_PASSWORD,
			Accounts:   map[string]string{user: pass},
			Address:    lo,
			UdpEnabled: true,
		}),
	}

	return &core.Config{
		App: []*serial.TypedMessage{
			serial.ToTypedMessage(&applog.Config{ErrorLogType: applog.LogType_None, AccessLogType: applog.LogType_None, ErrorLogLevel: clog.Severity_Unknown}),
			serial.ToTypedMessage(&dispatcher.Config{}),
			serial.ToTypedMessage(&proxyman.InboundConfig{}),
			serial.ToTypedMessage(&proxyman.OutboundConfig{}),
			// буфер на соединение 4 КБ вместо 512 КБ по умолчанию: голос и чат не упираются в него
			serial.ToTypedMessage(&policy.Config{Level: map[uint32]*policy.Policy{0: {Buffer: &policy.Policy_Buffer{Connection: 4 * 1024}}}}),
		},
		Inbound:  []*core.InboundHandlerConfig{inbound},
		Outbound: []*core.OutboundHandlerConfig{outbound},
	}, nil
}

func stopLocked() {
	if inst != nil {
		inst.Close()
		inst = nil
	}
}

//export XliteStart
func XliteStart(cfg *C.char, n C.int) C.int {
	if cfg == nil || n <= 0 || n > 65536 {
		return 1
	}
	text := C.GoStringN(cfg, n)
	kv, err := parseKV(text)
	if err != nil {
		return 1
	}
	config, err := build(kv)
	if err != nil {
		return 1
	}
	mu.Lock()
	defer mu.Unlock()
	stopLocked()
	in, err := core.New(config)
	if err != nil {
		return 2
	}
	if err := in.Start(); err != nil {
		in.Close()
		return 3
	}
	inst = in
	return 0
}

//export XliteStop
func XliteStop() {
	mu.Lock()
	stopLocked()
	mu.Unlock()
	runtime.GC()
	debug.FreeOSMemory() // вернуть освободившуюся кучу Go системе
}

var _ = unsafe.Pointer(nil)

func main() {}
