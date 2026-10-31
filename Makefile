SDK = org.kde.Sdk//6.11

.PHONY: all install appimage clean

all: install

wldim: wldim.cpp layer-shell.xml
	flatpak run --command=sh --filesystem=$(CURDIR) $(SDK) -c 'cd $(CURDIR) && \
		wayland-scanner client-header layer-shell.xml ls-header.h && \
		wayland-scanner private-code layer-shell.xml ls-code.c && \
		wayland-scanner client-header /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml xs-header.h && \
		wayland-scanner private-code /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml xs-code.c && \
		gcc -O2 -fPIC -c ls-code.c xs-code.c $$(pkg-config --cflags wayland-client) && \
		g++ -O2 -fPIC -c wldim.cpp $$(pkg-config --cflags Qt6Widgets wayland-client) && \
		g++ -o wldim wldim.o ls-code.o xs-code.o $$(pkg-config --libs Qt6Widgets wayland-client) -pthread'

install: wldim
	install -Dm755 wldim ~/.local/bin/wldim
	install -Dm644 share/com.wurztha.screendim.desktop ~/.local/share/applications/com.wurztha.screendim.desktop
	install -Dm644 share/com.wurztha.screendim.svg ~/.local/share/icons/hicolor/scalable/apps/com.wurztha.screendim.svg

appimage: wldim
	sh mkappdir.sh
	ARCH=x86_64 appimagetool AppDir Screen-Dim-x86_64.AppImage

clean:
	rm -rf wldim AppDir *.o *-header.h *-code.c Screen-Dim-x86_64.AppImage
