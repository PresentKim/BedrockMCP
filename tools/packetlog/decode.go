package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"reflect"
	"strings"

	"github.com/sandertv/gophertunnel/minecraft/protocol"
	"github.com/sandertv/gophertunnel/minecraft/protocol/packet"
)

type decoder struct {
	fromClient packet.Pool
	fromServer packet.Pool
	shieldID   int32
}

type decoded struct {
	ID        uint32
	Name      string
	Packet    packet.Packet
	Err       error
	Remaining int
}

func newDecoder() *decoder {
	return &decoder{fromClient: packet.NewClientPool(), fromServer: packet.NewServerPool()}
}

func (d *decoder) decode(rec record) (out decoded) {
	buf := bytes.NewBuffer(rec.Data)
	var header uint32
	if err := readVaruint32(buf, &header); err != nil {
		out.Err = err
		return out
	}
	out.ID = header & 0x3ff
	pool := d.fromClient
	if rec.fromServer() {
		pool = d.fromServer
	}
	factory, ok := pool[out.ID]
	if !ok {
		out.Name = fmt.Sprintf("Unknown(%d)", out.ID)
		out.Err = fmt.Errorf("packet id %d is not in the gophertunnel pool for this direction", out.ID)
		return out
	}
	pk := factory()
	out.Name = reflect.TypeOf(pk).Elem().Name()
	out.Packet = pk
	func() {
		defer func() {
			if r := recover(); r != nil {
				out.Err = fmt.Errorf("%v", r)
			}
		}()
		pk.Marshal(protocol.NewReader(buf, d.shieldID, false))
	}()
	out.Remaining = buf.Len()
	if registry, ok := pk.(*packet.ItemRegistry); ok && out.Err == nil {
		for _, item := range registry.Items {
			if item.Name == "minecraft:shield" {
				d.shieldID = int32(item.RuntimeID)
			}
		}
	}
	return out
}

func readVaruint32(buf *bytes.Buffer, x *uint32) error {
	var v uint32
	for i := uint(0); i < 35; i += 7 {
		b, err := buf.ReadByte()
		if err != nil {
			return fmt.Errorf("read packet header: %w", err)
		}
		v |= uint32(b&0x7f) << i
		if b&0x80 == 0 {
			*x = v
			return nil
		}
	}
	return fmt.Errorf("packet header varuint32 too long")
}

func runDecode(path string, skip, limit int, names, dir string, maxJSON int) error {
	d := newDecoder()
	filters := splitList(names)
	shown := 0
	return readLog(path, func(rec record) bool {
		pk := d.decode(rec)
		if rec.Seq <= skip {
			return true
		}
		if dir != "" && dir != rec.dirName() {
			return true
		}
		if len(filters) > 0 && !matchesAny(pk.Name, filters) {
			return true
		}
		body := ""
		if pk.Packet != nil {
			if data, err := json.Marshal(pk.Packet); err == nil {
				body = string(data)
			} else {
				body = "json: " + err.Error()
			}
		}
		if maxJSON > 0 && len(body) > maxJSON {
			body = body[:maxJSON] + fmt.Sprintf("... (%d bytes)", len(body))
		}
		line := fmt.Sprintf("#%d %s %s %s conn=%x %s id=%d size=%d", rec.Seq, rec.Time.Format("15:04:05.000"),
			rec.sideName(), rec.dirName(), rec.Conn, pk.Name, pk.ID, len(rec.Data))
		if pk.Err != nil {
			line += " decode_error=" + pk.Err.Error()
		} else if pk.Remaining > 0 {
			line += fmt.Sprintf(" unread_bytes=%d", pk.Remaining)
		}
		fmt.Println(line + " " + body)
		shown++
		return limit <= 0 || shown < limit
	})
}

func splitList(s string) []string {
	var out []string
	for _, part := range strings.Split(s, ",") {
		if part = strings.TrimSpace(part); part != "" {
			out = append(out, strings.ToLower(part))
		}
	}
	return out
}

func matchesAny(name string, filters []string) bool {
	lower := strings.ToLower(name)
	for _, f := range filters {
		if strings.Contains(lower, f) {
			return true
		}
	}
	return false
}
