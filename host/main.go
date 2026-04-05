//go:build windows

// main.go – Windows host service for ESP32 RemoteMouse.
//
// This program receives UDP packets from the ESP32 over Wi-Fi and injects
// them into the OS as real keyboard/mouse events using the Windows SendInput API.
//
// Usage:
//
//	remotemouse.exe install    – install and enable the Windows service (auto-start)
//	remotemouse.exe uninstall  – remove the Windows service
//	remotemouse.exe run        – run in foreground (debug mode)
//	(no args)                  – run as a Windows service (invoked by SCM)
package main

import (
	"fmt"
	"os"

	"remotemouse/inject"
	"remotemouse/receiver"
	"remotemouse/service"
)

func main() {
	if len(os.Args) < 2 {
		// Started by the Windows Service Control Manager
		if err := service.RunAsService(runLogic); err != nil {
			fmt.Fprintf(os.Stderr, "service.RunAsService: %v\n", err)
			os.Exit(1)
		}
		return
	}

	switch os.Args[1] {
	case "install":
		if err := service.Install(); err != nil {
			fmt.Fprintf(os.Stderr, "install: %v\n", err)
			os.Exit(1)
		}
		fmt.Println("Service installed and set to auto-start.")

	case "uninstall":
		if err := service.Uninstall(); err != nil {
			fmt.Fprintf(os.Stderr, "uninstall: %v\n", err)
			os.Exit(1)
		}
		fmt.Println("Service uninstalled.")

	case "run":
		// Foreground debug mode
		fmt.Println("Running in foreground mode (Ctrl-C to quit)...")
		if err := runLogic(); err != nil {
			fmt.Fprintf(os.Stderr, "run: %v\n", err)
			os.Exit(1)
		}

	default:
		fmt.Fprintf(os.Stderr, "Unknown command %q. Use: install | uninstall | run\n", os.Args[1])
		os.Exit(1)
	}
}

// runLogic is the core loop shared by both foreground and service modes.
// It starts the UDP receiver and feeds packets to the inject layer.
func runLogic() error {
	inj, err := inject.New()
	if err != nil {
		return fmt.Errorf("inject.New: %w", err)
	}
	defer inj.Close()

	return receiver.Listen(":10086", func(pkt receiver.HidPacket) {
		if err := inj.Handle(pkt); err != nil {
			fmt.Fprintf(os.Stderr, "inject error: %v\n", err)
		}
	})
}
