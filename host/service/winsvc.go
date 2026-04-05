//go:build windows

// Package service wraps the core application logic in a Windows Service that
// can be installed for automatic startup via the Service Control Manager (SCM).
//
// Service name: RemoteMouse
package service

import (
	"fmt"
	"os"
	"path/filepath"

	"golang.org/x/sys/windows/svc"
	"golang.org/x/sys/windows/svc/mgr"
)

const serviceName = "RemoteMouse"
const serviceDesc = "ESP32 RemoteMouse UDP receiver – injects keyboard/mouse events from ESP32"

// Install registers the service with the Windows SCM and sets it to auto-start.
func Install() error {
	exePath, err := os.Executable()
	if err != nil {
		return fmt.Errorf("os.Executable: %w", err)
	}
	exePath, err = filepath.Abs(exePath)
	if err != nil {
		return fmt.Errorf("filepath.Abs: %w", err)
	}

	m, err := mgr.Connect()
	if err != nil {
		return fmt.Errorf("mgr.Connect: %w", err)
	}
	defer m.Disconnect()

	// Check if service already exists
	s, err := m.OpenService(serviceName)
	if err == nil {
		s.Close()
		return fmt.Errorf("service %q already exists", serviceName)
	}

	s, err = m.CreateService(serviceName, exePath,
		mgr.Config{
			DisplayName: serviceName,
			Description: serviceDesc,
			StartType:   mgr.StartAutomatic,
		})
	if err != nil {
		return fmt.Errorf("mgr.CreateService: %w", err)
	}
	defer s.Close()
	return nil
}

// Uninstall removes the service from the Windows SCM.
func Uninstall() error {
	m, err := mgr.Connect()
	if err != nil {
		return fmt.Errorf("mgr.Connect: %w", err)
	}
	defer m.Disconnect()

	s, err := m.OpenService(serviceName)
	if err != nil {
		return fmt.Errorf("service %q not found: %w", serviceName, err)
	}
	defer s.Close()

	if err := s.Delete(); err != nil {
		return fmt.Errorf("s.Delete: %w", err)
	}
	return nil
}

// remotemouseSvc implements svc.Handler so we can run as a Windows Service.
type remotemouseSvc struct {
	run func() error
}

func (rs *remotemouseSvc) Execute(args []string, req <-chan svc.ChangeRequest,
	status chan<- svc.Status) (bool, uint32) {

	// Signal that we are starting
	status <- svc.Status{State: svc.StartPending}

	// Channel to receive errors from the run goroutine
	errCh := make(chan error, 1)
	go func() { errCh <- rs.run() }()

	status <- svc.Status{
		State:   svc.Running,
		Accepts: svc.AcceptStop | svc.AcceptShutdown,
	}

	for {
		select {
		case err := <-errCh:
			if err != nil {
				fmt.Fprintf(os.Stderr, "service run error: %v\n", err)
			}
			status <- svc.Status{State: svc.StopPending}
			return false, 0

		case c := <-req:
			switch c.Cmd {
			case svc.Stop, svc.Shutdown:
				status <- svc.Status{State: svc.StopPending}
				// The run() function will be interrupted when the underlying
				// UDP listener's connection is closed – we just return here.
				return false, 0
			default:
				status <- svc.Status{State: svc.Running,
					Accepts: svc.AcceptStop | svc.AcceptShutdown}
			}
		}
	}
}

// RunAsService runs the application under Windows SCM control.
// The provided run function is executed in a goroutine and may block.
func RunAsService(run func() error) error {
	return svc.Run(serviceName, &remotemouseSvc{run: run})
}
