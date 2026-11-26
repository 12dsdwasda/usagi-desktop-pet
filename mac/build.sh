#!/bin/bash
# 우사기 데스크톱 펫 macOS 빌드 (Xcode 명령줄 도구 필요). 결과: dist-mac/UsagiDesktopPet.app, dist-mac/UsagiDesktopPet-<ver>-mac.zip
set -euo pipefail
cd "$(dirname "$0")/.."
VER="${VER:-1.1.0}"
OUT=dist-mac
APP="$OUT/UsagiDesktopPet.app"
rm -rf "$OUT" build-mac
mkdir -p build-mac "$APP/Contents/MacOS" "$APP/Contents/Resources"

for arch in arm64 x86_64; do
  swiftc -O -target "$arch-apple-macos12.0" mac/Sources/*.swift -o "build-mac/usagi-$arch"
done
lipo -create build-mac/usagi-arm64 build-mac/usagi-x86_64 -output "$APP/Contents/MacOS/UsagiDesktopPet"

cp assets/sprites/walk.png assets/sprites/dance.png assets/sprites/rap.png assets/sprites/drag.png "$APP/Contents/Resources/"
iconutil -c icns mac/icon.iconset -o "$APP/Contents/Resources/AppIcon.icns"

cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>UsagiDesktopPet</string>
  <key>CFBundleIdentifier</key><string>io.github.12dsdwasda.usagi-desktop-pet</string>
  <key>CFBundleName</key><string>UsagiDesktopPet</string>
  <key>CFBundleDisplayName</key><string>우사기</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VER</string>
  <key>CFBundleVersion</key><string>$VER</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>LSMinimumSystemVersion</key><string>12.0</string>
  <key>LSUIElement</key><true/>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSAppleEventsUsageDescription</key><string>스포티파이·Music 에서 지금 나오는 곡을 읽고 재생을 조절해서 우사기가 따라 부르게 합니다.</string>
</dict>
</plist>
EOF

codesign --force --deep -s - "$APP"
(cd "$OUT" && ditto -c -k --keepParent UsagiDesktopPet.app "UsagiDesktopPet-$VER-mac.zip")
ls -la "$OUT"
lipo -info "$APP/Contents/MacOS/UsagiDesktopPet"
