package main

import (
	"bytes"
	"fmt"
	"reflect"

	"github.com/sandertv/gophertunnel/minecraft/protocol"
	"github.com/sandertv/gophertunnel/minecraft/protocol/packet"
)

type decoder struct {
	fromClient packet.Pool
	fromServer packet.Pool
	shieldID   int32
}

type decoded struct {
	Name      string
	Packet    packet.Packet
	Err       error
	Remaining int
}

func newDecoder() *decoder {
	return &decoder{fromClient: packet.NewClientPool(), fromServer: packet.NewServerPool()}
}

// Spyglass records Packet::write as outbound and Packet::read as inbound. In a local
// world the in-process server's writes are outbound too, so the other pool is tried
// when the expected one fails.
func (d *decoder) decode(id int16, body []byte, outbound bool) decoded {
	first, second := d.fromServer, d.fromClient
	if outbound {
		first, second = d.fromClient, d.fromServer
	}
	out := d.decodeWith(first, id, body)
	if out.Err != nil || out.Remaining > 0 {
		if alt := d.decodeWith(second, id, body); alt.Packet != nil && alt.Err == nil && alt.Remaining == 0 {
			out = alt
		}
	}
	if registry, ok := out.Packet.(*packet.ItemRegistry); ok && out.Err == nil {
		for _, item := range registry.Items {
			if item.Name == "minecraft:shield" {
				d.shieldID = int32(item.RuntimeID)
			}
		}
	}
	return out
}

func (d *decoder) decodeWith(pool packet.Pool, id int16, body []byte) (out decoded) {
	factory, ok := pool[uint32(id)]
	if !ok {
		out.Name = fmt.Sprintf("Unknown%d", id)
		out.Err = fmt.Errorf("gophertunnel has no packet with id %d", id)
		return out
	}
	pk := factory()
	out.Name = reflect.TypeOf(pk).Elem().Name()
	out.Packet = pk
	buf := bytes.NewBuffer(body)
	func() {
		defer func() {
			if r := recover(); r != nil {
				out.Err = fmt.Errorf("%v", r)
			}
		}()
		pk.Marshal(protocol.NewReader(buf, d.shieldID, false))
	}()
	out.Remaining = buf.Len()
	return out
}
