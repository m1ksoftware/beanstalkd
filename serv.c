#include <stdint.h>
#include <stdlib.h>
#include <sys/socket.h>
#include "dat.h"

struct Server srv = {
    Portdef,
    NULL,
    NULL,
    NULL,
    {
        Filesizedef,
    },
};


void
srvserve(Server *s)
{
    int r;
    Socket *sock;
    int64 period;

    if (sockinit() == -1) {
        twarnx("sockinit");
        exit(1);
    }

    s->inet.x = s;
    s->inet.f = (Handle)srvaccept_inet;
    s->local.x = s;
    s->local.f = (Handle)srvaccept_local;
    s->conns.less = (Less)connless;
    s->conns.rec = (Record)connrec;

    if (s->local.fd > 0) {
    	r = sockwant(&s->local, 'r');
    	if (r == -1) {
    		twarn("socwant local");
    		exit(2);
    	}
    }

    if (s->inet.fd > 0) {
    	r = sockwant(&s->inet, 'r');
    	if (r == -1) {
    		twarn("socwant inet");
    		exit(2);
    	}
    }

    for (;;) {
        period = prottick(s);

        int rw = socknext(&sock, period);
        if (rw == -1) {
            twarnx("socknext");
            exit(1);
        }

        if (rw) {
            sock->f(sock->x, rw);
        }
    }
}


void
srvaccept_inet(Server *s, int ev)
{
    h_accept(s->inet.fd, ev, s);
}

void
srvaccept_local(Server *s, int ev)
{
        h_accept(s->local.fd, ev, s);
}
