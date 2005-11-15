#if HAVE_CONFIG_H
# include "config.h"
#endif

#include <sys/types.h>
#ifdef __RCSID
__RCSID("$Id$");
#endif

#include <stdio.h>
#if HAVE_STDLIB_H
# include <stdlib.h>
#endif
#if HAVE_STRING_H
# include <string.h>
#endif
#if HAVE_UNISTD_H
# include <unistd.h>
#endif
#include <dirent.h>
#include <sys/stat.h>

#include "strupper.h"
#include "wl2k.h"
#include "timeout.h"

#define PROPLIMIT 5
#define WL2KBUF 2048
#define WL2K_TEMPFILE_TEMPLATE "/tmp/wl2k.XXXXXX"
#define PENDING "pending"

static int getrawchar(FILE *fp);
static int getcompressed(FILE *fp, FILE *ofp);
static struct proposal *parse_proposal(char *propline);
static int b2outboundproposal(FILE *fp, char *lastcommand, struct proposal **oproplist);
static void printprop(struct proposal *prop);
static void putcompressed(struct proposal *prop, FILE *fp);

struct proposal {
  char code;
  char type;
  char id[13];
  unsigned long usize;
  unsigned long csize;
  struct proposal *next;
  char *path;
  unsigned char *cdata;
};

static int
getrawchar(FILE *fp)
{
  int c;

  resettimeout();
  c = fgetc(fp);
  if (c == EOF) {
    fprintf(stderr, "lost connection in getrawchar()\n");
    exit(EXIT_FAILURE);
  }
  return c;
}

#define CHRNUL 0
#define CHRSOH 1
#define CHRSTX 2
#define CHREOT 4

static int
getcompressed(FILE *fp, FILE *ofp)
{
  int c;
  int len;
  int i;
  unsigned char title[81];
  unsigned char offset[7];
  int cksum = 0;

  c = getrawchar(fp);
  if (c != CHRSOH) {
    return WL2K_COMPRESSED_BAD;
  }
  len = getrawchar(fp);
  title[80] = '\0';
  for (i = 0; i < 80; i++) {
    c = getrawchar(fp);
    len--;
    title[i] = c;
    if (c == CHRNUL) {
      ungetc(c, fp);
      len++;
      break;
    }
  }
  c = getrawchar(fp);
  len--;
  if (c != CHRNUL) {
    return WL2K_COMPRESSED_BAD;
  }
  printf("title: %s\n", title);
  offset[6] = '\0';
  for (i = 0; i < 6; i++) {
    c = getrawchar(fp);
    len--;
    offset[i] = c;
    if (c == CHRNUL) {
      ungetc(c, fp);
      len++;
      break;
    }
  }
  c = getrawchar(fp);
  len--;
  if (c != CHRNUL) {
    return WL2K_COMPRESSED_BAD;
  }
  printf("offset: %s\n", offset);
  if (len != 0) {
    return WL2K_COMPRESSED_BAD;
  }
  if (strcmp(offset, "0") != 0) {
    /* XXX */
    return WL2K_COMPRESSED_BAD;
  }

  for (;;) {
    c = getrawchar(fp);
    switch (c) {
    case CHRSTX:
      printf("STX\n");
      len = getrawchar(fp);
      if (len == 0) {
	len = 256;
      }
      printf("len %d\n", len);
      while (len--) {
	c = getrawchar(fp);
	if (fputc(c, ofp) == EOF) {
	  perror("fputc()");
	  return WL2K_COMPRESSED_BAD;
	}
	cksum = (cksum + c) % 256;
      }
      break;
    case CHREOT:
      printf("EOT\n");
      c = getrawchar(fp);
      cksum = (cksum + c) % 256;
      if (cksum != 0) {
	fprintf(stderr, "bad cksum\n");
	return WL2K_COMPRESSED_BAD;
      }
      return WL2K_COMPRESSED_GOOD;
      break;
    default:
      fprintf(stderr, "unexpected character in compressed stream\n");
      return WL2K_COMPRESSED_BAD;
      break;
    }
  }
  return WL2K_COMPRESSED_BAD;
}

static void
putcompressed(struct proposal *prop, FILE *fp)
{
  int len;
  int i;
  unsigned char title[81];
  unsigned char offset[7];
  int cksum = 0;
  char *cp;
  long rem;
  int off = 0; /* XXX */

  strcpy(title, "test"); /* XXX */
  snprintf(offset, sizeof(offset), "%d", off); /* XXX */

  len = strlen(title) + strlen(offset) + 2;
  fprintf(fp, "%c%c%s%c%s%c", CHRSOH, len, title, CHRNUL, offset, CHRNUL);

  rem = prop->csize;
  cp = prop->cdata;

  if (rem < 6) {
    fprintf(stderr, "invalid compressed data\n");
    exit(EXIT_FAILURE);
  }
  fprintf(fp, "%c%c", CHRSTX, 6);
  for (i = 0; i < 6; i++) {
    cksum += *cp;
    fputc(*cp++, fp);
  }
  rem -= 6;

  cp += off;
  rem -= off;

  if (rem < 0) {
    fprintf(stderr, "invalid offset\n");
    exit(EXIT_FAILURE);
  }

  while (rem > 0) {
    printf("... %ld\n", rem);
    if (rem > 250) {
      len = 250;
    } else {
      len = rem;
    }
    fprintf(fp, "%c%c", CHRSTX, len);
    while (rem--) {
      cksum += *cp;
      fputc(*cp++, fp);
      len--;
    }
  }

  cksum = -cksum & 0xff;
  fprintf(fp, "%c%c", CHREOT, cksum);
}

static struct proposal *
parse_proposal(char *propline)
{
  char *cp = propline;
  static struct proposal prop;
  int i;
  char *endp;

  if (!cp) {
    return NULL;
  }
  if (*cp++ != 'F') {
    return NULL;
  }
  prop.code = *cp++;
  switch (prop.code) {
  case 'C':
    if (*cp++ != ' ') {
      fprintf(stderr, "malformed proposal 1\n");
      return NULL;
    }
    prop.type = *cp++;
    if ((prop.type != 'C') && (prop.type != 'E')) {
      fprintf(stderr, "malformed proposal 2\n");
      return NULL;
    }
    if (*cp++ != 'M') {
      fprintf(stderr, "malformed proposal 3\n");
      return NULL;
    }
    if (*cp++ != ' ') {
      fprintf(stderr, "malformed proposal 4\n");
      return NULL;
    }
    for (i = 0; i < 12; i++) {
      prop.id[i] = *cp++;
      if (prop.id[i] == ' ') {
	prop.id[i] = '\0';
	cp--;
	break;
      } else {
	if (prop.id[i] == '\0') {
	  fprintf(stderr, "malformed proposal 5\n");
	  return NULL;
	}
      }
    }
    prop.id[12] = '\0';
    if (*cp++ != ' ') {
      fprintf(stderr, "malformed proposal 6\n");
      return NULL;
    }
    prop.usize = strtoul(cp, &endp, 10);
    cp = endp;
    if (*cp++ != ' ') {
      fprintf(stderr, "malformed proposal 7\n");
      return NULL;
    }
    prop.csize = (unsigned int) strtoul(cp, &endp, 10);
    cp = endp;
    if (*cp != ' ') {
      fprintf(stderr, "malformed proposal 8\n");
      return NULL;
    }
    break;
  case 'A':
  case 'B':
  default:
    prop.type = 'X';
    prop.id[0] = '\0';
    prop.usize = 0;
    prop.csize = 0;
    break;
    fprintf(stderr, "unsupported proposal type %c\n", prop.code);
    break;
  }
  prop.next = NULL;
  prop.path = NULL;

  return &prop;
}

static void
printprop(struct proposal *prop)
{
  printf("proposal code %c type %c id %s usize %lu csize %lu next %p path %s cdata %p\n",
	 prop->code,
	 prop->type,
	 prop->id,
	 prop->usize,
	 prop->csize,
	 prop->next,
	 prop->path,
	 prop->cdata);
}

static struct proposal *
prepare_outbound_proposals(void)
{
  struct proposal *prop;
  struct proposal **opropnext;
  struct proposal *oproplist = NULL;
  DIR *dirp;
  struct dirent *dp;
  struct stat sb;
  char *sfn;
  FILE *sfp;
  char *tfn;
  char *cmd;

  opropnext = &oproplist;
  if ((dirp = opendir(PENDING)) == NULL) {
    perror("opendir()");
    exit(EXIT_FAILURE);
  }
  while ((dp = readdir(dirp)) != NULL) {
    if (dp->d_type != DT_REG) {
      continue;
    }
    if (strlen(dp->d_name) > 12) {
      fprintf(stderr,
	      "warning: skipping bad filename %s in pending directory %s\n",
	      dp->d_name, PENDING);
      continue;
    }
    printf("%s\n", dp->d_name);
    if ((prop = malloc(sizeof(struct proposal))) == NULL) {
      perror("malloc()");
      exit(EXIT_FAILURE);
    }
    prop->code = 'C';
    prop->type = 'E';
    strlcpy(prop->id, dp->d_name, 13);
    if (asprintf(&prop->path, "%s/%s", PENDING, dp->d_name) == -1) {
      perror("asprintf()");
      exit(EXIT_FAILURE);
    }

    if (stat(prop->path, &sb) != 0) {
      perror("stat()");
      exit(EXIT_FAILURE);
    }

    prop->usize = (unsigned long) sb.st_size;

    if ((sfn = strdup(WL2K_TEMPFILE_TEMPLATE)) == NULL) {
      perror("strdup()");
      exit(EXIT_FAILURE);
    }

    /* XXX */
    if ((tfn = mktemp(sfn)) == NULL) {
      perror(sfn);
      exit(EXIT_FAILURE);
    }

    if (asprintf(&cmd, "./lzhuf_1 e1 %s %s", prop->path, tfn) == -1) {
      perror("asprintf()");
      exit(EXIT_FAILURE);
    }
    if (system(cmd) != 0) {
      fprintf(stderr, "error uncompressing received data\n");
      exit(EXIT_FAILURE);
    }
    free(cmd);

    if (stat(tfn, &sb) != 0) {
      perror("stat()");
      exit(EXIT_FAILURE);
    }
    prop->csize = (unsigned long) sb.st_size;
    if ((prop->cdata = malloc(prop->csize * sizeof(unsigned char))) == NULL) {
      perror("malloc()");
      exit(EXIT_FAILURE);
    }

    if ((sfp = fopen(tfn, "r")) == NULL) {
      perror("fopen()");
      exit(EXIT_FAILURE);
    }

    printf("sfp %p prop->path %s tfn %s\n", sfp, prop->path, tfn);

    if (fread(prop->cdata, prop->csize, 1, sfp) != 1) {
      perror("fread()");
      exit(EXIT_FAILURE);
    }
    fclose(sfp);
    unlink(tfn);
    free(sfn);

    prop->next = NULL;

    *opropnext = prop;
    opropnext = &prop->next;
  }
  closedir(dirp);

  printf("---\n");

  for (prop = oproplist; prop != NULL; prop = prop->next) {
    printprop(prop);
  }

  return oproplist;
}

static int
b2outboundproposal(FILE *fp, char *lastcommand, struct proposal **oproplist)
{
  int i;
  char *sp;
  char *cp;
  int cksum = 0;
  char *line;
  struct proposal *prop;

  if (*oproplist) {
    prop = *oproplist;
    for (i = 0; i < PROPLIMIT; i++) {
      if (asprintf(&sp, "F%c %cM %s %lu %lu 0\r",
		  prop->code,
		  prop->type,
		  prop->id,
		  prop->usize,
		  prop->csize) == -1) {
	perror("asprintf()");
	exit(EXIT_FAILURE);
      }
      printf("%s\n", sp);
      fprintf(fp, "%s", sp);
      for (cp = sp; *cp; cp++) {
	cksum += (unsigned char) *cp;
      }
      free(sp);
      if ((prop = prop->next) == NULL) {
	break;
      }
    }
    cksum = -cksum & 0xff;
    printf("F> %2X\n", cksum);
    fprintf(fp, "F> %2X\r", cksum);
    if ((line = wl2kgetline(fp)) == NULL) {
      fprintf(stderr, "connection closed\n");
      exit(EXIT_FAILURE);
    }
    printf("proposal response: %s\n", line);
    /* XXX parse proposal response */

    prop = *oproplist;
    for (i = 0; i < PROPLIMIT; i++) {
      putcompressed(prop, fp);
      if ((prop = prop->next) == NULL) {
	break;
      }
    }
    *oproplist = prop;
    return 0;
  } else if (strncmp(lastcommand, "FF", 2) == 0) {
    fprintf(fp, "FQ\r\n");
    printf("FQ\n");
    return -1;
  } else {
    fprintf(fp, "FF\r\n");
    printf("FF\n");
    return 0;
  }
}

char *
wl2kgetline(FILE *fp)
{
  static char buf[WL2KBUF];
  int i;
  int c;

  for (i = 0; i < WL2KBUF; i++) {
    resettimeout();
    if ((c = fgetc(fp)) == EOF) {
      return NULL;
    }
    if (c == '\r') {
      buf[i] = '\0';
      return buf;
    }
    buf[i] = c;
  }
  return NULL;
}

void
wl2kexchange(FILE *fp)
{
  char *cp;
  int proposals = 0;
  int proposalcksum = 0;
  int i;
  const char *sid = "[PaclinkUNIX-1.0-B2FHM]";
  char *inboundsid = NULL;
  char *inboundsidcodes = NULL;
  char *line;
  struct proposal *prop;
  struct proposal ipropary[PROPLIMIT];
  struct proposal *oproplist;
  char *sfn;
  FILE *sfp;
  int fd = -1;
  char *cmd;
  unsigned long sentcksum;
  char *endp;

  oproplist = prepare_outbound_proposals();

  while ((line = wl2kgetline(fp)) != NULL) {
    printf("/%s/\n", line);
    if (line[0] == '[') {
      inboundsid = strdup(line);
      if ((cp = strrchr(inboundsid, '-')) == NULL) {
	fprintf(stderr, "bad sid %s\n", inboundsid);
	exit(EXIT_FAILURE);
      }
      inboundsidcodes = strdup(cp);
      if ((cp = strrchr(inboundsidcodes, ']')) == NULL) {
	fprintf(stderr, "bad sid %s\n", inboundsid);
	exit(EXIT_FAILURE);
      }
      *cp = '\0';
      strupper(inboundsidcodes);
      if (strstr(inboundsidcodes, "B2F") == NULL) {
	fprintf(stderr, "sid %s does not support B2F protocol\n", inboundsid);
	exit(EXIT_FAILURE);
      }
    } else if (line[strlen(line) - 1] == '>') {
      if (strchr(inboundsidcodes, 'I')) {
	/* XXX */
	/* printf("; %s DE %s QTC %d", remotecall, localcall, trafficcount);*/
      }
      fprintf(fp, "%s\r\n", sid);
      printf("%s\n", sid);
      break;
    }
  }
  if (line == NULL) {
    fprintf(stderr, "Lost connection. 1\n");
    exit(EXIT_FAILURE);
  }

  if (b2outboundproposal(fp, line, &oproplist) != 0) {
    return;
  }

  while ((line = wl2kgetline(fp)) != NULL) {
    printf("/%s/\n", line);
    if (strncmp(line, ";", 1) == 0) {
      /* do nothing */
    } else if (strncmp(line, "FC", 2) == 0) {
      for (cp = line; *cp; cp++) {
	proposalcksum += (unsigned char) *cp;
      }
      proposalcksum += '\r'; /* bletch */
      if (proposals == PROPLIMIT) {
	fprintf(stderr, "too many proposals\n");
	exit(EXIT_FAILURE);
      }
      /*
      if (bNeedAcknowledgement) {
	B2ConfirmSentMessages();
      }
      */
      if ((prop = parse_proposal(line)) == NULL) {
	fprintf(stderr, "failed to parse proposal\n");
	exit(EXIT_FAILURE);
      }
      memcpy(&ipropary[proposals], prop, sizeof(struct proposal));
      printprop(&ipropary[proposals]);
      proposals++;
    } else if (strncmp(line, "FF", 2) == 0) {
      /*
      if (bNeedAcknowledgement) {
	B2ConfirmSentMessages();
      }
      */
      if (b2outboundproposal(fp, line, &oproplist) != 0) {
	return;
      }
    } else if (strncmp(line, "S", 1) == 0) {
      /* Send("[441] - Command:""" & sCommand & """ not recognized - disconnecting") */
      return;
    } else if (strncmp(line, "B", 1) == 0) {
      return;
    } else if (strncmp(line, "FQ", 2) == 0) {
      /*
      if (bNeedAcknowledgement) {
	B2ConfirmSentMessages();
      }
      */
      return;
    } else if (strncmp(line, "F>", 2) == 0) {
      proposalcksum = (-proposalcksum) & 0xff;
      sentcksum = strtoul(line + 2, &endp, 16);

      if (sentcksum != (unsigned long) proposalcksum) {
	fprintf(stderr, "proposal cksum mismatch\n");
	exit(EXIT_FAILURE);
      }
      
      printf("%d proposals\n", proposals);

      if (proposals != 0) {
	fprintf(fp, "FS ");
	printf("FS ");
	for (i = 0; i < proposals; i++) {
	  if (ipropary[i].code == 'C') {
	    putc('Y', fp);
	    putchar('Y');
	  } else {
	    putc('N', fp);
	    putchar('N');
	  }
	}
	fprintf(fp, "\r\n");
	printf("\n");

	for (i = 0; i < proposals; i++) {
	  if (ipropary[i].code != 'C') {
	    continue;
	  }
	  if ((sfn = strdup(WL2K_TEMPFILE_TEMPLATE)) == NULL) {
	    perror("strdup()");
	    exit(EXIT_FAILURE);
	  }
	  if ((fd = mkstemp(sfn)) == -1 ||
	      (sfp = fdopen(fd, "w+")) == NULL) {
	    if (fd != -1) {
	      unlink(sfn);
	      close(fd);
	    }
	    perror(sfn);
	    exit(EXIT_FAILURE);
	  }

	  if (getcompressed(fp, sfp) != WL2K_COMPRESSED_GOOD) {
	    fprintf(stderr, "error receiving compressed data\n");
	    exit(EXIT_FAILURE);
	  }
	  if (fclose(sfp) != 0) {
	    fprintf(stderr, "error closing compressed data\n");
	    exit(EXIT_FAILURE);
	  }
	  printf("extracting...\n");
	  if (asprintf(&cmd, "./lzhuf_1 d1 %s %s", sfn, ipropary[i].id) == -1) {
	    perror("asprintf()");
	    exit(EXIT_FAILURE);
	  }
	  if (system(cmd) != 0) {
	    fprintf(stderr, "error uncompressing received data\n");
	    exit(EXIT_FAILURE);
	  }
	  free(cmd);
	  printf("\n");
	  printf("Finished!\n");
	  unlink(sfn);
#if 0
	  while ((line = wl2kgetline(fp)) != NULL) {
	    printf("%s\n", line);
	    if (line[0] == '\x1a') {
	      printf("yeeble\n");
	    }
	  }
	  if (line == NULL) {
	    fprintf(stderr, "Lost connection. 3\n");
	    exit(EXIT_FAILURE);
	  }
#endif
	}
      }
      proposals = 0;
      proposalcksum = 0;
      if (b2outboundproposal(fp, line, &oproplist) != 0) {
	return;
      }
    } else if (line[strlen(line - 1)] == '>') {
      /*
      if (bNeedAcknowledgement) {
	B2ConfirmSentMessages();
	if (b2outboundproposal(fp, line, &oproplist) != 0) {
	  return;
        }
      }
      */
    } else {
      fprintf(stderr, "unrecognized command: %s\n", line);
      exit(EXIT_FAILURE);
    }
  }
  if (line == NULL) {
    fprintf(stderr, "Lost connection. 4\n");
    exit(EXIT_FAILURE);
  }
}
