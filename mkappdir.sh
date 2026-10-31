#!/bin/sh
set -e
cd /var/home/wurztha/Desktop/OpenCode/Default/wldim
rm -rf AppDir
mkdir -p AppDir/usr/bin AppDir/usr/lib AppDir/usr/plugins AppDir/usr/share/applications

cp wldim AppDir/usr/bin/
cp ~/.local/share/icons/hicolor/scalable/apps/com.wurztha.screendim.svg AppDir/com.wurztha.screendim.svg
sed 's|^Exec=.*|Exec=wldim|' ~/.local/share/applications/com.wurztha.screendim.desktop > AppDir/com.wurztha.screendim.desktop
cp AppDir/com.wurztha.screendim.desktop AppDir/usr/share/applications/

# bundle runtime libraries (skip glibc / kernel / driver-provided ones)
ldd wldim | awk '/=> \// {print $3}' | sort -u | while read -r lib; do
    case "$lib" in
        */ld-linux*|*/libc.so*|*/libm.so*|*/libdl.so*|*/libpthread.so*|*/librt.so*|*/libnsl.so*|*/libutil.so*|*/libGL.so*|*/libGLX.so*|*/libGLdispatch.so*|*/libEGL.so*|*/libglib*|*/libgio*|*/libgobject*|*/libgthread*|*/libgmodule*|*/libsystemd*|*/libselinux*|*/libsepol*|*/libpcre2*|*/libffi.so*|*/libdbus*|*/libdrm*|*/libX11*|*/libXxcb*|*/libxcb*|*/libXext*|*/libXrender*|*/libXi*|*/libXcursor*|*/libXfixes*|*/libXrandr*|*/libgomp*)
            continue ;;
    esac
    cp -n "$lib" AppDir/usr/lib/
done

# Qt plugins needed at runtime
for p in platforms/libqwayland.so platforms/libqxcb.so imageformats/libqsvg.so iconengines/libqsvgicon.so; do
    src=$(find /usr/lib -path "*/plugins/$p" 2>/dev/null | head -1)
    [ -n "$src" ] || { echo "MISSING $p"; continue; }
    mkdir -p "AppDir/usr/plugins/$(dirname $p)"
    cp "$src" "AppDir/usr/plugins/$p"
done

# Qt wayland sub-plugins (shell integration, graphics, decorations)
for d in wayland-shell-integration wayland-graphics-integration-client wayland-decoration-client; do
    src=$(find /usr/lib -type d -name "$d" -path "*plugins*" 2>/dev/null | head -1)
    [ -n "$src" ] || { echo "MISSING dir $d"; continue; }
    mkdir -p "AppDir/usr/plugins/$d"
    cp "$src"/*.so "AppDir/usr/plugins/$d/" 2>/dev/null
done

# bundle every dependency the plugins need that is not already present
for f in AppDir/usr/bin/wldim AppDir/usr/plugins/*/*.so; do
    ldd "$f" 2>/dev/null | awk '/=> \// {print $3}' | while read -r lib; do
        case "$lib" in
            */ld-linux*|*/libc.so*|*/libm.so*|*/libdl.so*|*/libpthread.so*|*/librt.so*|*/libnsl.so*|*/libutil.so*|*/libGL.so*|*/libGLX.so*|*/libGLdispatch.so*|*/libOpenGL.so*|*/libEGL.so*|*/libglib*|*/libgio*|*/libgobject*|*/libgthread*|*/libgmodule*|*/libsystemd*|*/libselinux*|*/libsepol*|*/libpcre2*|*/libffi.so*|*/libdbus*|*/libdrm*|*/libX*|*/libxcb*|*/libgomp*|*/AppDir/*) continue ;;
        esac
        cp -n "$lib" AppDir/usr/lib/ 2>/dev/null
    done
done

cat > AppDir/AppRun <<'EOF'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-wayland;xcb}"
export QT_PLUGIN_PATH="$HERE/usr/plugins"
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$HERE/usr/bin/wldim" "$@"
EOF
chmod +x AppDir/AppRun
echo APPDIR-OK
