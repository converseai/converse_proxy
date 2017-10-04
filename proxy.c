#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <errno.h>
#include <signal.h>

#define MAX_EVENTS 20
#define BUFF_LEN 4096

int epollfd, listenfd;
int proxyport, serverport;
char *proxyip, *serverip;
struct sockaddr_in *serveraddr, *listenaddr;

typedef struct forward
{
  int fd;
  struct forward *to;
} forward;


void getsock (int *s);
int dobind (int *s);
int dolisten (int *s);
int backendconnect ();
int doforward (int from, int to);
int setnonblocking (int fd);
void runpoller ();

static volatile sig_atomic_t proxy_sigquit = 0;

static void
handlesignal (int sig)
{
  switch (sig)
    {
    case SIGINT:
    case SIGTERM:
      proxy_sigquit++;
      break;
    default:
      break;
    }
}

static void
setsignalmasks ()
{
  struct sigaction sa;
  /* Set up the structure to specify the new action. */
  memset (&sa, 0, sizeof (struct sigaction));
  sa.sa_handler = handlesignal;
  sigemptyset (&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction (SIGINT, &sa, NULL);
  sigaction (SIGTERM, &sa, NULL);

  memset (&sa, 0, sizeof (struct sigaction));
  sa.sa_handler = SIG_IGN;
  sa.sa_flags = SA_RESTART;
  sigaction (SIGPIPE, &sa, NULL);
}

static void
usage ()
{
  fprintf (stderr,
	   "Usage: converse_proxy [-l <listenip:port>] [-f <forwardip:port>] \n");
  exit (EXIT_FAILURE);
}

static void
processinput (int argc, char *argv[])
{
  int opt = 0;
  char *proxyipport, *serveripport;
  proxyipport = serveripport = NULL;

  while ((opt = getopt (argc, argv, "l:f:")) != -1)
    {
      switch (opt)
	{
	case 'l':
	  proxyipport = strdup (optarg);
	  break;
	case 'f':
	  serveripport = strdup (optarg);
	  break;
	default:
	  usage ();

	}
    }

  if ((proxyipport == NULL) || (serveripport == NULL))
    {
      usage ();
    }
  char *colon = strrchr (proxyipport, ':');
  char *port = NULL;
  if (colon)
    {
      *colon = 0;
      port = colon + 1;
    }
  else
    {
      usage ();
    }
  proxyip = proxyipport;
  proxyport = atoi (port);

  colon = strrchr (serveripport, ':');
  port = NULL;
  if (colon)
    {
      *colon = 0;
      port = colon + 1;
    }
  else
    {
      usage ();
    }

  serverip = serveripport;
  serverport = atoi (port);
}

int
main (int argc, char *argv[])
{
  processinput (argc, argv);
  setsignalmasks ();
  // create epoll fd
  epollfd = epoll_create (10);
  if (epollfd == -1)
    {
      perror ("Epoll create error");
      exit (1);
    }
  // add the listen fd
  getsock (&listenfd);
  if (!(listenfd > 0))
    {
      perror ("Listener socket error");
      exit (1);
    }
  int arg = 1;
  setsockopt (listenfd, SOL_SOCKET, SO_REUSEADDR, &arg, sizeof (arg));

  struct hostent *hp;
  hp = NULL;

  hp = gethostbyname (proxyip);
  if (hp == NULL)
    {
      perror ("Invalid proxy ip");
      exit (1);
    }
  listenaddr = (struct sockaddr_in *) calloc (1, sizeof (struct sockaddr_in));
  listenaddr->sin_family = AF_INET;
  listenaddr->sin_port = htons (proxyport);
  bcopy (hp->h_addr, (char *) &listenaddr->sin_addr, hp->h_length);

  hp = gethostbyname (serverip);
  if (hp == NULL)
    {
      perror ("Invalid server ip");
      exit (1);
    }
  //Setup the two sock address up
  //Backend  server 
  serveraddr = (struct sockaddr_in *) calloc (1, sizeof (struct sockaddr_in));
  serveraddr->sin_family = AF_INET;
  serveraddr->sin_port = htons (serverport);
  bcopy (hp->h_addr, (char *) &serveraddr->sin_addr, hp->h_length);

  if (setnonblocking (listenfd) == -1)
    {
      perror ("Setting proxyfd non blocking");
      exit (1);
    }

  if (dobind (&listenfd) == -1)
    {
      perror ("Error on bind");
      exit (1);
    }

  if (dolisten (&listenfd) == -1)
    {
      perror ("Error on listen");
      exit (1);
    }

  // Now set socket to epoll
  static struct epoll_event event;
  event.events = EPOLLIN | EPOLLERR;
  event.data.fd = listenfd;

  if (epoll_ctl (epollfd, EPOLL_CTL_ADD, listenfd, &event) == -1)
    {
      perror ("Error adding listerner to epoll");
      exit (1);
    }
  runpoller ();
}

/*
Get a new socket
*/
void
getsock (int *s)
{
  int temp;
  temp = socket (AF_INET, SOCK_STREAM, 0);
  *s = temp;
  return;
}

int
setnonblocking (int fd)
{
  int oldflags = fcntl (fd, F_GETFL, 0);
  /* If reading the flags failed, return error indication now. */
  if (oldflags == -1)
    return -1;
  /* Set just the flag we want to set. */
  oldflags |= O_NONBLOCK;
  /* Store modified flag word in the descriptor. */
  return fcntl (fd, F_SETFL, oldflags);
}

/*
Do a new bind
*/
int
dobind (int *s)
{
  int rc;
  int temps = *s;
  rc = bind (temps, listenaddr, sizeof (*listenaddr));
  return rc;
}

/*
do the listen
*/
int
dolisten (int *s)
{
  int rc;
  int temps;
  temps = *s;
  rc = listen (temps, 10);	/* backlog of 10 */
  return rc;
}

/*
Connect to the backend server
*/
int
backendconnect ()
{
  int sock;
  getsock (&sock);
  if (sock == -1)
    {
      return -1;
    }

  if (connect (sock, serveraddr, sizeof (*serveraddr)) == -1)
    {
      return -1;
    }
  setnonblocking (sock);
  return sock;
}

void
runpoller ()
{
  struct epoll_event ev, events[MAX_EVENTS];
  int nfds, n;
  int accept_fd;
  int new_fd;
  struct sockaddr_in addr;
  socklen_t alen = sizeof (struct sockaddr_in);

  for (;;)
    {
      nfds = epoll_wait (epollfd, events, MAX_EVENTS, 2000);
      //printf ("FD wake %d\n", nfds);
      if (nfds == -1)
	{
	  if ((errno != EAGAIN) && (errno != EWOULDBLOCK))
	    {
	      perror ("epoll_wait");
	      exit (EXIT_FAILURE);
	    }
	  continue;
	}
      if (proxy_sigquit > 0)
	{
	  break;
	}
      for (n = 0; n < nfds; n++)
	{
	  if (events[n].data.fd == listenfd)
	    {
	      // new connection
	      accept_fd =
		accept (listenfd, ((struct sockaddr *) &addr), &alen);
	      if (accept_fd == -1)
		{
		  if ((errno != EAGAIN) && (errno != EWOULDBLOCK))
		    {
		      perror ("epoll_wait");
		      exit (EXIT_FAILURE);
		    }
		  continue;
		}
	      //printf ("Accept done %d\n", accept_fd);
	      setnonblocking (accept_fd);
	      new_fd = backendconnect ();
	      if (new_fd != -1)
		{
		  //printf ("Backend connected %d\n", new_fd);
		  ev.events = EPOLLIN | EPOLLET;
		  //ev.data.fd = new_fd;
		  forward *afw, *bfw;
		  afw = bfw = NULL;
		  afw = calloc (1, sizeof (forward));
		  afw->fd = new_fd;
		  ev.data.ptr = (void *) afw;
		  if (epoll_ctl (epollfd, EPOLL_CTL_ADD, new_fd, &ev) == 0)
		    {
		      //printf ("epoll connected %d\n", new_fd);
		      // Add the accept fd to epoll 
		      //ev.data.fd = accept_fd;
		      bfw = calloc (1, sizeof (forward));
		      bfw->fd = accept_fd;
		      bfw->to = afw;
		      afw->to = bfw;
		      ev.data.ptr = (void *) bfw;
		      if (epoll_ctl (epollfd, EPOLL_CTL_ADD, accept_fd, &ev)
			  == 0)
			{
			  //printf ("epoll connected %d\n", accept_fd);
			  // All good here
			}
		      else
			{
			  perror ("epoll_ctl accept_fd");
			  free (bfw);
			  close (accept_fd);
			  // clean up this fd will be happenig on doforward
			  close (new_fd);
			}
		    }
		  else
		    {
		      // Epoll failed
		      perror ("epoll_ctl new_fd");
		      free (afw);
		      close (new_fd);
		      close (accept_fd);
		    }
		}
	      else
		{
		  close (accept_fd);
		}
	    }
	  else
	    {
	      // forward event
	      ev = events[n];
	      forward *fw;
	      fw = (forward *) ev.data.ptr;
	      //printf ("epoll event connected %d -> %d\n", fw->fd, fw->to->fd);
	      if (doforward (fw->fd, fw->to->fd) == -1)
		{
		  //fprintf (stdout, "Cleaning fd");
		  close (fw->fd);
		  close (fw->to->fd);
		  free (fw->to);
		  free (fw);
		}
	    }
	}
    }
  close (epollfd);
}

int
doforward (int from, int to)
{
  char data[BUFF_LEN];
  int rc;
  int wc;
  int n;
  rc = wc = n = 0;
  int fwfd;
  while (1)
    {
      memset (data, '0', BUFF_LEN);
      rc = read (from, data, BUFF_LEN);
      if (rc > 0)
	{
	  // Now write this data to the other socket
	  wc = rc;
	  while (wc > 0)
	    {
	      n = write (to, data, wc);
	      if (n == -1)
		{
		  if ((errno != EAGAIN) && (errno != EWOULDBLOCK))
		    {
		      return -1;
		    }
		}
	      else
		{
		  wc -= n;
		}
	    }
	}
      else if (rc == 0)		// EOF
	{
	  return -1;
	}
      else
	{
	  if ((errno != EAGAIN) && (errno != EWOULDBLOCK))
	    {
	      return -1;
	    }
	  // All read back to main loop
	  return 0;
	}
    }
}
