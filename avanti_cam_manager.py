#!/usr/bin/env python3
"""
=============================================================================
Avanti / Arenti / Tuya PetCam - All-in-One Local Manager
=============================================================================
Features:
1. WiFi Pairing QR-Code Generator (GUI & ANSI-Terminal)
   - Standard WiFi format (WIFI:S:..;T:..;P:..;;)
   - Tuya/Arenti JSON format ({"s":"..","p":".."})
2. RTSP Live Stream (OpenCV)
3. ONVIF PTZ Camera Control (Keyboard controls during live stream)
4. Local Network Discovery (ONVIF/RTSP port scan)

VENV Quickstart:
----------------
  python3 -m venv venv
  source venv/bin/activate
  pip install opencv-python qrcode[pil] onvif-zeep pillow

Usage:
------
  python avanti_cam_manager.py --pair
  python avanti_cam_manager.py --stream --ip 192.168.1.150 --user admin --password secret
  python avanti_cam_manager.py --interactive
=============================================================================
"""

import sys
import os
import time
import argparse
import socket
import threading
from typing import Optional, Tuple

try:
    import cv2
except ImportError:
    cv2 = None

try:
    import qrcode
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    qrcode = None

try:
    from onvif import ONVIFCamera
except ImportError:
    ONVIFCamera = None


# =============================================================================
# QR Code Generator
# =============================================================================
def generate_pairing_qr(ssid: str, password: str, format_type: str = "all") -> None:
    """
    Generates pairing QR codes for the camera in standard WiFi and Tuya/Arenti JSON format.
    Renders both in terminal (ASCII) and as graphical popups.
    """
    if qrcode is None:
        print("[ERROR] 'qrcode' module not found. Run: pip install qrcode[pil]")
        return

    payloads = []
    
    # 1. Standard Wi-Fi pairing format
    wifi_str = f"WIFI:S:{ssid};T:WPA;P:{password};;"
    payloads.append(("Standard Wi-Fi Format", wifi_str))

    # 2. Tuya / Arenti JSON pairing format
    tuya_str = f'{{"s":"{ssid}","p":"{password}"}}'
    payloads.append(("Arenti / Tuya JSON Format", tuya_str))

    print("\n" + "=" * 60)
    print("  WLAN-PAIRING QR-CODE GENERATOR")
    print("=" * 60)
    print(f"SSID:     {ssid}")
    print(f"Passwort: {'*' * len(password)}")
    print("Hinweis:  Halten Sie den QR-Code ca. 15-20 cm vor die Kameralinse,")
    print("          sobald die Kamera nach dem Einschalten/Reset piept.\n")

    for title, payload in payloads:
        if format_type != "all" and format_type.lower() not in title.lower():
            continue

        print("-" * 60)
        print(f"Format: {title}")
        print(f"Payload: {payload}")
        print("-" * 60)

        # Terminal ANSI output
        qr = qrcode.QRCode(
            version=1,
            error_correction=qrcode.constants.ERROR_CORRECT_M,
            box_size=1,
            border=2,
        )
        qr.add_data(payload)
        qr.make(fit=True)
        qr.print_ascii(invert=True)

        # Graphical Popup if GUI available
        try:
            img_qr = qr.make_image(fill_color="black", back_color="white")
            canvas_w, canvas_h = 450, 520
            canvas = Image.new("RGB", (canvas_w, canvas_h), "white")
            
            qr_display = img_qr.resize((380, 380))
            canvas.paste(qr_display, (35, 60))

            draw = ImageDraw.Draw(canvas)
            draw.text((35, 20), f"Avanti Setup: {title}", fill="black")
            draw.text((35, 460), f"SSID: {ssid}", fill="black")
            draw.text((35, 485), "Vor die Kameralinse halten...", fill="gray")

            if cv2 is not None:
                import numpy as np
                cv_img = cv2.cvtColor(np.array(canvas), cv2.COLOR_RGB2BGR)
                win_name = f"Pairing: {title}"
                cv2.imshow(win_name, cv_img)
                print(f"[INFO] Fenster '{win_name}' geöffnet. Taste drücken zum Fortfahren...")
                cv2.waitKey(0)
                cv2.destroyWindow(win_name)
            else:
                canvas.show()
        except Exception as e:
            print(f"[WARN] Grafische Anzeige nicht möglich: {e}")


# =============================================================================
# ONVIF PTZ Controller
# =============================================================================
class CameraPTZController:
    """
    Handles ONVIF PTZ (Pan / Tilt / Zoom) movements.
    """
    def __init__(self, ip: str, port: int, user: str, password: str):
        self.ip = ip
        self.port = port
        self.user = user
        self.password = password
        self.camera = None
        self.ptz = None
        self.profile = None
        self.is_connected = False
        self._init_connection()

    def _init_connection(self) -> None:
        if ONVIFCamera is None:
            print("[WARN] 'onvif-zeep' nicht installiert. PTZ-Steuerung deaktiviert.")
            return

        try:
            print(f"[ONVIF] Verbinde zu http://{self.ip}:{self.port} als '{self.user}'...")
            self.camera = ONVIFCamera(self.ip, self.port, self.user, self.password)
            self.media = self.camera.create_media_service()
            self.ptz = self.camera.create_ptz_service()
            
            profiles = self.media.GetProfiles()
            if not profiles:
                print("[ONVIF ERROR] Keine Media-Profile gefunden.")
                return
            
            self.profile = profiles[0]
            self.is_connected = True
            print(f"[ONVIF] Erfolgreich verbunden! Profil: {self.profile.token}")
        except Exception as e:
            print(f"[ONVIF ERROR] Verbindung fehlgeschlagen: {e}")
            self.is_connected = False

    def move(self, pan_speed: float, tilt_speed: float) -> None:
        """
        Sends continuous move command.
        pan_speed:  -1.0 (links) bis 1.0 (rechts)
        tilt_speed: -1.0 (runter) bis 1.0 (hoch)
        """
        if not self.is_connected or not self.ptz:
            return

        try:
            request = {
                'ProfileToken': self.profile.token,
                'Velocity': {
                    'PanTilt': {'x': pan_speed, 'y': tilt_speed}
                }
            }
            self.ptz.ContinuousMove(request)
        except Exception as e:
            print(f"[PTZ ERROR] Move failed: {e}")

    def stop(self) -> None:
        """Stops any ongoing PTZ movement."""
        if not self.is_connected or not self.ptz:
            return

        try:
            self.ptz.Stop({'ProfileToken': self.profile.token, 'PanTilt': True, 'Zoom': True})
        except Exception as e:
            print(f"[PTZ ERROR] Stop failed: {e}")


# =============================================================================
# RTSP Stream & Keyboard Loop
# =============================================================================
def run_live_stream(ip: str, rtsp_port: int, rtsp_path: str, user: str, password: str,
                   onvif_port: int = 8000) -> None:
    """
    Opens the RTSP live stream using OpenCV and maps keyboard input to ONVIF PTZ motions.
    """
    if cv2 is None:
        print("[ERROR] 'opencv-python' nicht gefunden. Run: pip install opencv-python")
        return

    # Build RTSP URL
    if user and password:
        rtsp_url = f"rtsp://{user}:{password}@{ip}:{rtsp_port}/{rtsp_path.lstrip('/')}"
    else:
        rtsp_url = f"rtsp://{ip}:{rtsp_port}/{rtsp_path.lstrip('/')}"

    print(f"\n[RTSP] Öffne Stream: rtsp://{user}:****@{ip}:{rtsp_port}/{rtsp_path.lstrip('/')}")
    
    # Initialize PTZ controller
    ptz_controller = CameraPTZController(ip, onvif_port, user, password)

    # Open VideoCapture with TCP transport
    os.environ["OPENCV_FFMPEG_CAPTURE_OPTIONS"] = "rtsp_transport;tcp"
    cap = cv2.VideoCapture(rtsp_url, cv2.CAP_FFMPEG)

    if not cap.isOpened():
        print(f"[ERROR] Stream konnte nicht geöffnet werden.")
        print(f"Überprüfen Sie:")
        print(f" - Ist die IP '{ip}' korrekt erreichbar?")
        print(f" - Stimmt der RTSP-Pfad? (Gängig: 'live/ch0', 'Streaming/Channels/101', 'h264Preview_01_main')")
        print(f" - Wurde 'PC-View' / 'ONVIF' in den Kamera-Einstellungen aktiviert?")
        return

    print("\n" + "=" * 60)
    print("  LIVE-STREAM & PTZ-STEUERUNG AKTIV")
    print("=" * 60)
    print("  Tastenbelegung im Videofenster:")
    print("    [W] / [Pfeil Oben]   : Kamera nach oben neigen (Tilt Up)")
    print("    [S] / [Pfeil Unten]  : Kamera nach unten neigen (Tilt Down)")
    print("    [A] / [Pfeil Links]  : Kamera nach links drehen (Pan Left)")
    print("    [D] / [Pfeil Rechts] : Kamera nach rechts drehen (Pan Right)")
    print("    [LEERTASTE]          : Bewegung stoppen")
    print("    [Q] / [ESC]          : Beenden")
    print("=" * 60 + "\n")

    window_name = f"Avanti PetCam Live Stream - {ip}"
    cv2.namedWindow(window_name, cv2.WINDOW_NORMAL)
    cv2.resizeWindow(window_name, 960, 540)

    speed = 0.5
    last_key_time = time.time()
    moving = False

    while True:
        ret, frame = cap.read()
        if not ret:
            print("[RTSP WARN] Kein Frame empfangen. Reconnecte...")
            time.sleep(0.5)
            continue

        # Draw HUD Overlay on frame
        status_text = f"IP: {ip} | PTZ: {'ON' if ptz_controller.is_connected else 'OFF'}"
        cv2.putText(frame, status_text, (20, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2)
        cv2.putText(frame, "W/A/S/D: Bewegen | Leertaste: Stop | Q: Beenden", 
                    (20, frame.shape[0] - 20), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 1)

        cv2.imshow(window_name, frame)

        key = cv2.waitKey(1) & 0xFF

        # Exit
        if key in [ord('q'), ord('Q'), 27]:  # 27 = ESC
            break

        # PTZ Navigation
        if key == ord('w') or key == 82:  # W or Arrow Up
            print("[PTZ] Up")
            ptz_controller.move(0.0, speed)
            moving = True
            last_key_time = time.time()
        elif key == ord('s') or key == 84:  # S or Arrow Down
            print("[PTZ] Down")
            ptz_controller.move(0.0, -speed)
            moving = True
            last_key_time = time.time()
        elif key == ord('a') or key == 81:  # A or Arrow Left
            print("[PTZ] Left")
            ptz_controller.move(-speed, 0.0)
            moving = True
            last_key_time = time.time()
        elif key == ord('d') or key == 83:  # D or Arrow Right
            print("[PTZ] Right")
            ptz_controller.move(speed, 0.0)
            moving = True
            last_key_time = time.time()
        elif key == 32:  # Space
            print("[PTZ] Stop")
            ptz_controller.stop()
            moving = False

    ptz_controller.stop()
    cap.release()
    cv2.destroyAllWindows()
    print("[INFO] Stream beendet.")


# =============================================================================
# Fast Subnet Port Scanner (Find camera IP)
# =============================================================================
def scan_network_for_cameras(subnet_prefix: str) -> None:
    """
    Scans the local subnet for typical ONVIF (8000, 8080) and RTSP (554, 8554) ports.
    """
    print(f"\n[SCAN] Scanne Subnetz {subnet_prefix}.1 - {subnet_prefix}.254 nach Kameras...")
    ports = [554, 8554, 8000, 8080]
    found = []

    def check_ip(ip_str: str):
        for port in ports:
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.settimeout(0.3)
                res = s.connect_ex((ip_str, port))
                s.close()
                if res == 0:
                    found.append((ip_str, port))
                    print(f"  -> Gefunden: {ip_str}:{port} (Offen)")
            except Exception:
                pass

    threads = []
    for i in range(1, 255):
        ip = f"{subnet_prefix}.{i}"
        t = threading.Thread(target=check_ip, args=(ip,))
        threads.append(t)
        t.start()

    for t in threads:
        t.join()

    print(f"[SCAN] Abgeschlossen. {len(found)} offene Ports gefunden.\n")


# =============================================================================
# Interactive CLI Menu
# =============================================================================
def interactive_menu():
    print("=" * 60)
    print("  AVANTI / ARENTI PETCAM LINUX CONTROLLER")
    print("=" * 60)
    print("1) WLAN-Pairing QR-Code erstellen")
    print("2) Lokales Netzwerk nach Kameras scannen")
    print("3) Live-Stream & PTZ-Steuerung starten")
    print("4) Beenden")
    print("-" * 60)

    choice = input("Auswahl [1-4]: ").strip()

    if choice == "1":
        ssid = input("WLAN-Name (SSID): ").strip()
        pwd = input("WLAN-Passwort: ").strip()
        generate_pairing_qr(ssid, pwd)
    elif choice == "2":
        prefix = input("Subnetz-Präfix (z.B. 192.168.1 oder 192.168.178): ").strip()
        scan_network_for_cameras(prefix)
    elif choice == "3":
        ip = input("Kamera IP-Adresse: ").strip()
        user = input("Benutzername [admin]: ").strip() or "admin"
        pwd = input("Passwort: ").strip()
        rtsp_port = int(input("RTSP-Port [8554]: ").strip() or "8554")
        rtsp_path = input("RTSP-Pfad [Streaming/Channels/101]: ").strip() or "Streaming/Channels/101"
        onvif_port = int(input("ONVIF-Port [8000]: ").strip() or "8000")
        run_live_stream(ip, rtsp_port, rtsp_path, user, pwd, onvif_port)
    elif choice == "4":
        sys.exit(0)


# =============================================================================
# Main Entry Point
# =============================================================================
def main():
    parser = argparse.ArgumentParser(description="Avanti PetCam Local Provisioning, Stream & PTZ Controller")
    parser.add_argument("--interactive", action="store_true", help="Interaktives Menü starten")
    parser.add_argument("--pair", action="store_true", help="WLAN Pairing QR-Code erzeugen")
    parser.add_argument("--ssid", type=str, help="WLAN SSID")
    parser.add_argument("--wifi-pwd", type=str, help="WLAN Passwort")
    parser.add_argument("--scan", type=str, help="Subnetz scannen (z.B. 192.168.1)")
    
    parser.add_argument("--stream", action="store_true", help="Live-Stream & PTZ starten")
    parser.add_argument("--ip", type=str, help="Kamera IP-Adresse")
    parser.add_argument("--user", type=str, default="admin", help="Kamera Benutzername (Standard: admin)")
    parser.add_argument("--password", type=str, default="", help="Kamera Passwort")
    parser.add_argument("--rtsp-port", type=int, default=8554, help="RTSP Port (Standard: 8554)")
    parser.add_argument("--rtsp-path", type=str, default="Streaming/Channels/101", help="RTSP Pfad")
    parser.add_argument("--onvif-port", type=int, default=8000, help="ONVIF Port (Standard: 8000)")

    args = parser.parse_args()

    if args.pair:
        ssid = args.ssid or input("WLAN SSID: ")
        pwd = args.wifi_pwd or input("WLAN Passwort: ")
        generate_pairing_qr(ssid, pwd)
    elif args.scan:
        scan_network_for_cameras(args.scan)
    elif args.stream:
        if not args.ip:
            print("[ERROR] Bitte geben Sie die IP mit --ip an.")
            return
        run_live_stream(args.ip, args.rtsp_port, args.rtsp_path, args.user, args.password, args.onvif_port)
    else:
        interactive_menu()


if __name__ == "__main__":
    main()

