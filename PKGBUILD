# Maintainer: Thomas Lübking <thomas.luebking at gmail>

pkgname=qarma
pkgver=0.1.0
pkgrel=1
pkgdesc="A drop-in replacement clone for zenity, written in Qt4/5"
arch=('i686' 'x86_64')
url="https://github.com/luebking/qarma"
license=('GPL')

depends=('qt4')
makedepends=('gcc')
license=('GPL')

build()
{
    qmake .. && make || return 1
    mkdir -p $pkgdir/usr/bin
    install qarma $pkgdir/usr/bin
    strip $pkgdir/usr/bin/qarma
}

# vim:set ts=2 sw=2 et: