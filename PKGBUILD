pkgname=libfprint
pkgver=1.94.100
pkgrel=4
pkgdesc="Library for fingerprint readers (with Samsung Notebook FPC1025 driver)"
url="https://fprint.freedesktop.org/"
arch=(x86_64)
license=(LGPL-2.1-or-later)
depends=(
  libgcc
  glib2
  glibc
  libgudev
  libgusb
  openssl
  pixman
)
provides=(libfprint-2.so)
conflicts=(libfprint)
source=(
  "git+https://gitlab.freedesktop.org/libfprint/libfprint.git#tag=v$pkgver"
  "0001-fpc1021-samsung.patch"
)
sha256sums=(
  'SKIP'
  'SKIP'
)

prepare() {
  cd "$pkgname"
  git apply ../0001-fpc1021-samsung.patch
}

build() {
  meson setup build "$pkgname" \
    --prefix=/usr \
    --buildtype=release \
    -D drivers=all \
    -D introspection=true \
    -D doc=false \
    -D installed-tests=false

  meson compile -C build
}

package() {
  DESTDIR="$pkgdir" meson install -C build
}
