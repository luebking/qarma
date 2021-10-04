# Maintainer: Thomas Lübking <thomas.luebking at gmail>

pkgname=qarma
pkgver=r71.968836b
pkgrel=1
pkgdesc="A drop-in replacement clone for zenity, written in Qt5/6"
arch=('i686' 'x86_64')
url="https://github.com/luebking/qarma"
license=('GPL')
depends=('qt5-base')
makedepends=('git' 'gcc')

pkgver()
{
    printf "r%s.%s" "$(git rev-list --count HEAD)" "$(git rev-parse --short HEAD)"
}

build()
{
    qmake-qt5 ..
    make
}

package()
{
    install -Dm755 qarma -t "$pkgdir/usr/bin"
    ln -s /usr/bin/qarma "$pkgdir/usr/bin/qarma-askpass"
}

# vim:set ts=2 sw=2 et:
