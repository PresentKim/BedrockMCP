package main

import (
	"flag"
	"fmt"
	"os"
)

func main() {
	if len(os.Args) < 2 {
		usage()
	}
	switch os.Args[1] {
	case "decode":
		fs := flag.NewFlagSet("decode", flag.ExitOnError)
		file := fs.String("file", "", "packet log (.bmpl)")
		skip := fs.Int("skip", 0, "skip records up to this sequence number")
		limit := fs.Int("limit", 200, "maximum packets to print, 0 for all")
		names := fs.String("name", "", "comma-separated packet name substrings")
		dir := fs.String("dir", "", "sent or received")
		maxJSON := fs.Int("max-json", 2000, "truncate each packet's JSON, 0 for no limit")
		_ = fs.Parse(os.Args[2:])
		exit(runDecode(*file, *skip, *limit, *names, *dir, *maxJSON))
	case "packs":
		fs := flag.NewFlagSet("packs", flag.ExitOnError)
		file := fs.String("file", "", "packet log (.bmpl)")
		out := fs.String("out", "", "output directory")
		_ = fs.Parse(os.Args[2:])
		exit(runPacks(*file, *out))
	default:
		usage()
	}
}

func usage() {
	fmt.Fprintln(os.Stderr, "usage: packetlog decode -file LOG [-skip N] [-limit N] [-name a,b] [-dir sent|received] [-max-json N]")
	fmt.Fprintln(os.Stderr, "       packetlog packs -file LOG -out DIR")
	os.Exit(2)
}

func exit(err error) {
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
