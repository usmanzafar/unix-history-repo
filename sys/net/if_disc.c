/*-
 * Copyright (c) 1982, 1986, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	From: @(#)if_loop.c	8.1 (Berkeley) 6/10/93
 * $FreeBSD$
 */

/*
 * Discard interface driver for protocol testing and timing.
 * (Based on the loopback.)
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/sockio.h>

#include <net/if.h>
#include <net/if_clone.h>
#include <net/if_types.h>
#include <net/route.h>
#include <net/bpf.h>

#include "opt_inet.h"
#include "opt_inet6.h"

#ifdef TINY_DSMTU
#define	DSMTU	(1024+512)
#else
#define DSMTU	65532
#endif

#define DISCNAME	"disc"

struct disc_softc {
	struct ifnet *sc_ifp;
};

static int	discoutput(struct ifnet *, struct mbuf *,
		    struct sockaddr *, struct rtentry *);
static void	discrtrequest(int, struct rtentry *, struct rt_addrinfo *);
static int	discioctl(struct ifnet *, u_long, caddr_t);
static int	disc_clone_create(struct if_clone *, int, caddr_t);
static void	disc_clone_destroy(struct ifnet *);

static MALLOC_DEFINE(M_DISC, DISCNAME, "Discard interface");

IFC_SIMPLE_DECLARE(disc, 0);

static int
disc_clone_create(struct if_clone *ifc, int unit, caddr_t params)
{
	struct ifnet		*ifp;
	struct disc_softc	*sc;

	sc = malloc(sizeof(struct disc_softc), M_DISC, M_WAITOK | M_ZERO);
	ifp = sc->sc_ifp = if_alloc(IFT_LOOP);
	if (ifp == NULL) {
		free(sc, M_DISC);
		return (ENOSPC);
	}

	ifp->if_softc = sc;
	if_initname(ifp, ifc->ifc_name, unit);
	ifp->if_mtu = DSMTU;
	/*
	 * IFF_LOOPBACK should not be removed from disc's flags because
	 * it controls what PF-specific routes are magically added when
	 * a network address is assigned to the interface.  Things just
	 * won't work as intended w/o such routes because the output
	 * interface selection for a packet is totally route-driven.
	 * A valid alternative to IFF_LOOPBACK can be IFF_BROADCAST or
	 * IFF_POINTOPOINT, but it would result in different properties
	 * of the interface.
	 */
	ifp->if_flags = IFF_LOOPBACK | IFF_MULTICAST;
	ifp->if_drv_flags = IFF_DRV_RUNNING;
	ifp->if_ioctl = discioctl;
	ifp->if_output = discoutput;
	ifp->if_hdrlen = 0;
	ifp->if_addrlen = 0;
	ifp->if_snd.ifq_maxlen = 20;
	if_attach(ifp);
	bpfattach(ifp, DLT_NULL, sizeof(u_int32_t));

	return (0);
}

static void
disc_clone_destroy(struct ifnet *ifp)
{
	struct disc_softc	*sc;

	sc = ifp->if_softc;

	bpfdetach(ifp);
	if_detach(ifp);
	if_free(ifp);

	free(sc, M_DISC);
}

static int
disc_modevent(module_t mod, int type, void *data)
{

	switch (type) {
	case MOD_LOAD:
		if_clone_attach(&disc_cloner);
		break;
	case MOD_UNLOAD:
		if_clone_detach(&disc_cloner);
		break;
	default:
		return (EOPNOTSUPP);
	}
	return (0);
}

static moduledata_t disc_mod = {
	"if_disc",
	disc_modevent,
	NULL
};

DECLARE_MODULE(if_disc, disc_mod, SI_SUB_PSEUDO, SI_ORDER_ANY);

static int
discoutput(struct ifnet *ifp, struct mbuf *m, struct sockaddr *dst,
    struct rtentry *rt)
{
	u_int32_t af;

	M_ASSERTPKTHDR(m);

	/* BPF writes need to be handled specially. */
	if (dst->sa_family == AF_UNSPEC) {
		bcopy(dst->sa_data, &af, sizeof(af));
		dst->sa_family = af;
	}

	if (bpf_peers_present(ifp->if_bpf)) {
		u_int af = dst->sa_family;
		bpf_mtap2(ifp->if_bpf, &af, sizeof(af), m);
	}
	m->m_pkthdr.rcvif = ifp;

	ifp->if_opackets++;
	ifp->if_obytes += m->m_pkthdr.len;

	m_freem(m);
	return (0);
}

/* ARGSUSED */
static void
discrtrequest(int cmd, struct rtentry *rt, struct rt_addrinfo *info)
{
	RT_LOCK_ASSERT(rt);
	rt->rt_rmx.rmx_mtu = DSMTU;
}

/*
 * Process an ioctl request.
 */
static int
discioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	struct ifaddr *ifa;
	struct ifreq *ifr = (struct ifreq *)data;
	int error = 0;

	switch (cmd) {

	case SIOCSIFADDR:
		ifp->if_flags |= IFF_UP;
		ifa = (struct ifaddr *)data;
		if (ifa != 0)
			ifa->ifa_rtrequest = discrtrequest;
		/*
		 * Everything else is done at a higher level.
		 */
		break;

	case SIOCADDMULTI:
	case SIOCDELMULTI:
		if (ifr == 0) {
			error = EAFNOSUPPORT;		/* XXX */
			break;
		}
		switch (ifr->ifr_addr.sa_family) {

#ifdef INET
		case AF_INET:
			break;
#endif
#ifdef INET6
		case AF_INET6:
			break;
#endif

		default:
			error = EAFNOSUPPORT;
			break;
		}
		break;

	case SIOCSIFMTU:
		ifp->if_mtu = ifr->ifr_mtu;
		break;

	default:
		error = EINVAL;
	}
	return (error);
}
