package main

import (
	"encoding/hex"
	"encoding/json"
	"fmt"
	"reflect"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/sandertv/gophertunnel/minecraft/protocol/packet"
)

type filter struct {
	names  []string
	ids    map[int16]bool
	dir    string
	failed bool
	unread bool
	from   uint64
	to     uint64
}

func parseFilter(names, ids, dir string, failed, unread bool, from, to uint64) (filter, error) {
	f := filter{names: splitList(names), dir: dir, failed: failed, unread: unread, from: from, to: to}
	if dir != "" && dir != "in" && dir != "out" {
		return f, fmt.Errorf("dir must be in or out")
	}
	for _, part := range splitList(ids) {
		base := 10
		if strings.HasPrefix(part, "0x") {
			base, part = 16, part[2:]
		}
		v, err := strconv.ParseInt(part, base, 16)
		if err != nil {
			return f, fmt.Errorf("bad packet id %q", part)
		}
		if f.ids == nil {
			f.ids = map[int16]bool{}
		}
		f.ids[int16(v)] = true
	}
	return f, nil
}

func (f filter) match(e entry, name string) bool {
	if f.dir != "" && f.dir != e.dirName() {
		return false
	}
	if f.failed && !e.failed() {
		return false
	}
	if f.unread && e.Unread == 0 {
		return false
	}
	if f.ids != nil && !f.ids[e.ID] {
		return false
	}
	return len(f.names) == 0 || matchesAny(name, f.names)
}

var (
	clientPool = packet.NewClientPool()
	serverPool = packet.NewServerPool()
)

func nameOf(e entry) string {
	pool := serverPool
	if e.outbound() {
		pool = clientPool
	}
	factory, ok := pool[uint32(e.ID)]
	if !ok {
		if factory, ok = clientPool[uint32(e.ID)]; !ok {
			if factory, ok = serverPool[uint32(e.ID)]; !ok {
				return fmt.Sprintf("Unknown%d", e.ID)
			}
		}
	}
	return reflect.TypeOf(factory()).Elem().Name()
}

type printer struct {
	cap     *capture
	dec     *decoder
	fields  bool
	maxJSON int
	primed  bool
}

func (p *printer) prime() {
	if p.primed {
		return
	}
	p.primed = true
	_ = p.cap.entries(1, 0, func(e entry) bool {
		if e.ID != int16(packet.IDItemRegistry) || e.outbound() {
			return true
		}
		if b, err := p.cap.read(e); err == nil {
			p.dec.decode(e.ID, b.Body, false)
		}
		return false
	})
}

func (p *printer) line(e entry) string {
	name := nameOf(e)
	out := fmt.Sprintf("#%d +%.3fs %s %s id=%d len=%d", e.Number, float64(e.Micros)/1e6, e.dirName(), name, e.ID, e.BodyLen)
	if e.Unread > 0 {
		out += fmt.Sprintf(" client_unread=%d", e.Unread)
	}
	var b blob
	var readErr error
	if e.failed() || e.hasError() || p.fields {
		b, readErr = p.cap.read(e)
	}
	if e.failed() {
		out += " CLIENT_DECODE_FAILED"
	}
	if e.hasError() && readErr == nil {
		out += " reason=" + reasonOf(b.Error)
	}
	if p.fields && readErr == nil {
		p.prime()
		pk := p.dec.decode(e.ID, b.Body, e.outbound())
		if pk.Err != nil {
			out += " gophertunnel_error=" + quoteLine(pk.Err.Error())
		} else if pk.Remaining > 0 {
			out += fmt.Sprintf(" gophertunnel_unread=%d", pk.Remaining)
		}
		if pk.Packet != nil && pk.Err == nil {
			out += " " + truncate(jsonOf(pk.Packet), p.maxJSON)
		}
	}
	return out
}

func reasonOf(doc any) string {
	if m, ok := doc.(map[string]any); ok {
		if r, ok := m["reason"].(string); ok {
			return quoteLine(r)
		}
	}
	return quoteLine(jsonOf(doc))
}

func quoteLine(s string) string {
	return strconv.Quote(s)
}

func jsonOf(v any) string {
	data, err := json.Marshal(v)
	if err != nil {
		return "json error: " + err.Error()
	}
	return string(data)
}

func truncate(s string, max int) string {
	if max > 0 && len(s) > max {
		return s[:max] + fmt.Sprintf("...(%d chars)", len(s))
	}
	return s
}

func runInfo(c *capture) error {
	total, err := c.count()
	if err != nil {
		return err
	}
	counts := map[string]int{}
	failed := 0
	_ = c.entries(1, 0, func(e entry) bool {
		counts[e.dirName()+" "+nameOf(e)]++
		if e.failed() {
			failed++
		}
		return true
	})
	fmt.Printf("capture %s (client %s): %d packets, %d the client failed to decode\n", c.Path, c.Client, total, failed)
	type kv struct {
		k string
		v int
	}
	var list []kv
	for k, v := range counts {
		list = append(list, kv{k, v})
	}
	sort.Slice(list, func(a, b int) bool { return list[a].v > list[b].v })
	for i, item := range list {
		if i == 25 {
			fmt.Printf("... %d more packet types\n", len(list)-25)
			break
		}
		fmt.Printf("  %6d %s\n", item.v, item.k)
	}
	return nil
}

func runFind(c *capture, f filter, last, limit int, p *printer) error {
	var hits []entry
	err := c.entries(f.from, f.to, func(e entry) bool {
		if f.match(e, nameOf(e)) {
			hits = append(hits, e)
			if last <= 0 && limit > 0 && len(hits) > limit {
				return false
			}
			if last > 0 && len(hits) > last {
				hits = hits[1:]
			}
		}
		return true
	})
	if err != nil {
		return err
	}
	more := false
	if last <= 0 && limit > 0 && len(hits) > limit {
		hits, more = hits[:limit], true
	}
	total, _ := c.count()
	fmt.Printf("%d matching packets shown (capture has %d)\n", len(hits), total)
	for _, e := range hits {
		fmt.Println(p.line(e))
	}
	if more {
		fmt.Printf("... more matches after #%d; continue with from=%d\n", hits[len(hits)-1].Number, hits[len(hits)-1].Number+1)
	}
	return nil
}

func runGet(c *capture, number uint64, maxHex int) error {
	var found *entry
	_ = c.entries(number, number, func(e entry) bool { found = &e; return false })
	if found == nil {
		return fmt.Errorf("no packet #%d", number)
	}
	e := *found
	b, err := c.read(e)
	if err != nil {
		return err
	}
	p := &printer{cap: c, dec: newDecoder()}
	p.prime()
	fmt.Println(p.line(e))
	pk := p.dec.decode(e.ID, b.Body, e.outbound())
	if pk.Packet != nil && pk.Err == nil {
		data, _ := json.MarshalIndent(pk.Packet, "", "  ")
		fmt.Printf("fields (gophertunnel %s):\n%s\n", pk.Name, data)
	} else {
		fmt.Printf("gophertunnel could not decode it: %v\n", pk.Err)
	}
	if pk.Remaining > 0 {
		fmt.Printf("gophertunnel left %d bytes unread\n", pk.Remaining)
	}
	if b.Error != nil {
		data, _ := json.MarshalIndent(b.Error, "", "  ")
		fmt.Printf("client error (spyglass):\n%s\n", data)
	}
	if b.Fields != nil {
		data, _ := json.MarshalIndent(b.Fields, "", "  ")
		fmt.Printf("client fields (spyglass):\n%s\n", data)
	}
	body := b.Body
	suffix := ""
	if maxHex > 0 && len(body) > maxHex {
		body, suffix = body[:maxHex], fmt.Sprintf("\n... %d more bytes", len(b.Body)-maxHex)
	}
	fmt.Printf("body (%d bytes, without the varuint header):\n%s%s\n", len(b.Body), hex.Dump(body), suffix)
	return nil
}

func runWait(c *capture, f filter, after int64, timeout time.Duration, p *printer) error {
	start := uint64(0)
	if after >= 0 {
		start = uint64(after)
	} else if total, err := c.count(); err == nil {
		start = total
	}
	deadline := time.Now().Add(timeout)
	next := start + 1
	for {
		var hit *entry
		_ = c.entries(next, 0, func(e entry) bool {
			next = e.Number + 1
			if f.match(e, nameOf(e)) {
				hit = &e
				return false
			}
			return true
		})
		if hit != nil {
			fmt.Println(p.line(*hit))
			return nil
		}
		if time.Now().After(deadline) {
			return fmt.Errorf("no matching packet after #%d within %s; checked up to #%d, call again with after=%d", start, timeout, next-1, next-1)
		}
		time.Sleep(100 * time.Millisecond)
	}
}

func runFailures(c *capture) error {
	type group struct {
		name, reason string
		count        int
		first, last  uint64
	}
	groups := map[string]*group{}
	var order []string
	err := c.entries(1, 0, func(e entry) bool {
		if !e.failed() && !e.hasError() {
			return true
		}
		reason := "(no reason recorded)"
		if b, err := c.read(e); err == nil && b.Error != nil {
			reason = reasonOf(b.Error)
		}
		key := e.dirName() + " " + nameOf(e) + " " + reason
		g, ok := groups[key]
		if !ok {
			g = &group{name: e.dirName() + " " + nameOf(e), reason: reason, first: e.Number}
			groups[key] = g
			order = append(order, key)
		}
		g.count++
		g.last = e.Number
		return true
	})
	if err != nil {
		return err
	}
	if len(order) == 0 {
		fmt.Println("the client decoded every captured packet")
		return nil
	}
	for _, key := range order {
		g := groups[key]
		fmt.Printf("%d x %s reason=%s first=#%d last=#%d\n", g.count, g.name, g.reason, g.first, g.last)
	}
	fmt.Println("use get with a packet number for the call stack and bytes")
	return nil
}
