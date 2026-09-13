# onvif_test.py
from onvif import ONVIFCamera

# Tragen Sie hier das Passwort ein (oder 'admin' / leer lassen zum Testen)
cam_ip = '192.168.1.7'
cam_port = 8000
cam_user = 'admin'
cam_pass = 'admin'  # ggf. anpassen

try:
    cam = ONVIFCamera(cam_ip, cam_port, cam_user, cam_pass)
    media = cam.create_media_service()
    profiles = media.GetProfiles()
    
    print(f"ONVIF-Verbindung erfolgreich! Gefundene Profile: {len(profiles)}")
    for profile in profiles:
        stream_uri = media.GetStreamUri({
            'StreamSetup': {'Stream': 'RTP-Unicast', 'Transport': {'Protocol': 'RTSP'}},
            'ProfileToken': profile.token
        })
        print(f"  -> Profil: '{profile.Name}' ({profile.token})")
        print(f"     Stream-URL: {stream_uri.Uri}")

except Exception as e:
    print(f"Fehler bei ONVIF-Abfrage: {e}")

