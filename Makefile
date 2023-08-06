MK_DEBUG_FILES?= no
LOCALBASE?= /usr/local
SUBDIR=	plugins rc.d
BINDIR=	$(LOCALBASE)/sbin
MANDIR= $(LOCALBASE)/man/man
DIRS=	VMCONFDIR UEFIVARDIR
VMCONFDIR=$(LOCALBASE)/etc/bmd.d
UEFIVARDIR=$(LOCALBASE)/var/cache/bmd
PROG=	bmd
MAN=	bmd.8 bmdctl.8 bmd.conf.5
LINKS=  ${BINDIR}/bmd ${BINDIR}/bmdctl
SRCS=	bmd.c conf.c tap.c parser.c vm.c server.c control.c inspect.c global.c \
	console.c inspect_grub.c confparse.h confparse.y conflex.l y.tab.h
CFLAGS+=-Wall -DLOCALBASE=\"$(LOCALBASE)\"
LDADD=	-lnv
LDFLAGS=-Xlinker -dynamic-list=export.symbols
INCS=bmd_plugin.h
INCSDIR=$(LOCALBASE)/include

CLEANFILES= y.output

.include <bsd.prog.mk>
