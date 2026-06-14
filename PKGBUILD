# Maintainer: Kira (Sigmachan) <senedato@gmail.com>
pkgname=wlgame-git
pkgver=r1.g$(git -C . rev-parse --short HEAD 2>/dev/null || echo 0000000)
pkgrel=1
pkgdesc='Gaming-focused Wayland compositor — all staging protocols + NVIDIA auto-detect'
arch=(x86_64)
url='https://github.com/Sigmachan/wlgame'
license=(MIT)
depends=(
  wlroots-git
  wayland
  libxkbcommon
)
makedepends=(
  meson
  ninja
  wayland-protocols-git
  git
)
provides=(wlgame)
conflicts=(wlgame)
source=("$pkgname::git+file://$PWD")
sha256sums=(SKIP)

pkgver() {
  cd "$pkgname"
  printf "r%s.g%s" "$(git rev-list --count HEAD)" "$(git rev-parse --short HEAD)"
}

build() {
  arch-meson "$pkgname" build
  meson compile -C build
}

package() {
  meson install -C build --destdir "$pkgdir"
  install -Dm644 "$pkgname/LICENSE" "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}
