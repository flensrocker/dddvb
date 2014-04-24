/*
/* ddflash - Programmer for flash on Digital Devices devices
 *
 * Copyright (C) 2013 Digital Devices GmbH
 *                    Ralph Metzler <rmetzler@digitaldevices.de>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 3 only, as published by the Free Software Foundation.
 *
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA
 * Or, point your browser to http://www.gnu.org/copyleft/gpl.html
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/types.h>

static int reboot(uint32_t off)
{
	FILE *f;
	uint32_t time;

	if ((f = fopen ("/sys/class/rtc/rtc0/since_epoch", "r")) == NULL)
		return -1;
	fscanf(f, "%u", &time);
	fclose(f);

	if ((f = fopen ("/sys/class/rtc/rtc0/wakealarm", "r+")) == NULL)
		return -1;
	fprintf(f, "%u", time + off);
	fclose(f);
	system("/sbin/poweroff");
	return 0;
}

#define DDB_MAGIC 'd'

struct ddb_id {
	__u16 vendor;
	__u16 device;
	__u16 subvendor;
	__u16 subdevice;
	__u32 hw;
	__u32 regmap;
};

struct ddb_flashio {
	__u8 *write_buf;
	__u32 write_len;
	__u8 *read_buf;
	__u32 read_len;
};

#define IOCTL_DDB_FLASHIO  _IOWR(DDB_MAGIC, 0x00, struct ddb_flashio)
#define IOCTL_DDB_ID       _IOR(DDB_MAGIC, 0x03, struct ddb_id)


struct ddflash {
	int fd;
	struct ddb_id id;
	uint32_t type;

	uint32_t sector_size;
	uint32_t size;

	uint32_t bufsize;
	uint32_t offset;
	uint32_t block_erase;

	uint8_t * buffer;
};

int flashio(int ddb, uint8_t *wbuf, uint32_t wlen, uint8_t *rbuf, uint32_t rlen)
{
	struct ddb_flashio fio = {
		.write_buf=wbuf,
		.write_len=wlen,
		.read_buf=rbuf,
		.read_len=rlen,
	};
	
	return ioctl(ddb, IOCTL_DDB_FLASHIO, &fio);
}

enum {
	UNKNOWN_FLASH = 0,
	ATMEL_AT45DB642D = 1,
	SSTI_SST25VF016B = 2,
	SSTI_SST25VF032B = 3,
};

static int flashread(int ddb, uint8_t *buf, uint32_t addr, uint32_t len)
{
	uint8_t cmd[4]= {0x03, (addr >> 16) & 0xff, 
			 (addr >> 8) & 0xff, addr & 0xff};
	
	return flashio(ddb, cmd, 4, buf, len);
}

static int flashdump(struct ddflash *ddf, uint32_t addr, uint32_t len)
{
	int i, j;
	uint8_t buf[32];
	int bl = sizeof(buf);
	
	for (j = 0; j < len; j += bl, addr += bl) {
		flashread(ddf->fd, buf, addr, bl);
		for (i = 0; i < bl; i++) {
			printf("%02x ", buf[i]);
		}
		printf("\n");
	}
}

void dump(const uint8_t *b, int l)
{
	int i, j;
	
	for (j = 0; j < l; j += 16, b += 16) { 
		for (i = 0; i < 16; i++)
			if (i + j < l)
				printf("%02x ", b[i]);
			else
				printf("   ");
		printf(" | ");
		for (i = 0; i < 16; i++)
			if (i + j < l)
				putchar((b[i] > 31 && b[i] < 127) ? b[i] : '.');
		printf("\n");
	}
}

static int flashwrite_SSTI(struct ddflash *ddf, int fs, uint32_t FlashOffset, uint32_t maxlen)
{
    int err = 0;
    uint8_t cmd[6];
    int i, j;
    uint32_t flen, blen;

    blen = flen = lseek(fs, 0, SEEK_END);
    if (blen % 0xfff)
	    blen = (blen + 0xfff) & 0xfffff000; 
    printf("blen = %u, flen = %u\n", blen, flen);
    do {
#if 1
	    cmd[0] = 0x50;  // EWSR
	    err = flashio(ddf->fd, cmd, 1, NULL, 0);
	    if (err < 0) 
		    break;

	    cmd[0] = 0x01;  // WRSR
	    cmd[1] = 0x00;  // BPx = 0, Unlock all blocks
	    err = flashio(ddf->fd, cmd, 2, NULL, 0);
	    if (err < 0 )
		    break;
	    
	    for (i = 0; i < flen; i += 4096) {
		    if ((i & 0xFFFF) == 0 )
			    printf("Erase %08x\n", FlashOffset + i);
		    cmd[0] = 0x06;  // WREN
		    err = flashio(ddf->fd, cmd, 1, NULL, 0);
		    if (err < 0 )
			    break;
		    
		    cmd[0] = 0x20;  // Sector erase ( 4Kb)
		    cmd[1] = (((FlashOffset + i ) >> 16) & 0xFF);
		    cmd[2] = (((FlashOffset + i ) >>  8) & 0xFF);
		    cmd[3] = 0x00;
		    err = flashio(ddf->fd,cmd,4,NULL,0);
		    if (err < 0 )
			    break;
		    
		    while(1) {
			    cmd[0] = 0x05;  // RDRS
			    err = flashio(ddf->fd,cmd,1,&cmd[0],1);
			    if (err < 0 ) break;
			    if ((cmd[0] & 0x01) == 0 ) break;
		    }
		    if (err < 0 ) break;
	    }
	    if (err < 0 ) 
		    break;
#endif
	    for (j = blen - 4096; j >= 0; j -= 4096 ) {
		    uint32_t len = 4096; 
		    ssize_t rlen;
		    
		    if (lseek(fs, j, SEEK_SET) < 0) {
			    printf("seek error\n");
			    return -1;
		    }
		    if (flen - j < 4096) {
			    len = flen - j;
			    memset(ddf->buffer, 0xff, 4096);
		    }
   		    rlen = read(fs, ddf->buffer, len);
		    if (rlen < 0 || rlen != len) {
			    printf("file read error %d,%d at %u\n", rlen, errno, j);
			    return -1;
		    }
		    printf ("write %u bytes at %08x\n", len, j);

		    if ((j & 0xFFFF) == 0 )
			    printf(" Program  %08x\n",FlashOffset + j);
#if 1		    
		    for (i = 0; i < 4096; i += 2) {
			    if (i == 0) {
				    cmd[0] = 0x06;  // WREN
				    err = flashio(ddf->fd, cmd, 1, NULL, 0);
				    if (err < 0 ) 
					    break;
				    
				    cmd[0] = 0xAD;  // AAI
				    cmd[1] = ((( FlashOffset + j ) >> 16) & 0xFF );
				    cmd[2] = ((( FlashOffset + j ) >>  8) & 0xFF );
				    cmd[3] = 0x00;
				    cmd[4] = ddf->buffer[i];
				    cmd[5] = ddf->buffer[i + 1];
				    err = flashio(ddf->fd,cmd,6,NULL,0);
			    } else {
				    cmd[0] = 0xAD;  // AAI
				    cmd[1] = ddf->buffer[i];
				    cmd[2] = ddf->buffer[i + 1];
				    err = flashio(ddf->fd,cmd,3,NULL,0);
			    }
			    if (err < 0 ) 
				    break;
			    
			    while(1) {
				    cmd[0] = 0x05;  // RDRS
				    err = flashio(ddf->fd,cmd,1,&cmd[0],1);
				    if (err < 0 ) break;
				    if ((cmd[0] & 0x01) == 0 ) break;
			    }
			    if (err < 0 ) 
				    break;
		    }
		    if (err < 0)
			    break;
		    
		    cmd[0] = 0x04;  // WDIS
		    err = flashio(ddf->fd, cmd, 1, NULL, 0);
		    if (err < 0 ) 
			    break;
#endif
	    }
	    if (err < 0 ) break;
	    
	    cmd[0] = 0x50;  // EWSR
	    err = flashio(ddf->fd,cmd,1,NULL,0);
	    if (err < 0 ) break;
	    
	    cmd[0] = 0x01;  // WRSR
	    cmd[1] = 0x1C;  // BPx = 0, Lock all blocks
	    err = flashio(ddf->fd,cmd,2,NULL,0);
    } while(0);
    return err;
}


static int flashwrite(struct ddflash *ddf, int fs, uint32_t addr, uint32_t maxlen)
{
	flashwrite_SSTI(ddf, fs, addr, maxlen);
}

static int flashcmp(struct ddflash *ddf, int fs, uint32_t addr, uint32_t maxlen)
{
	uint32_t len;
	int i, j, rlen;
	uint8_t buf[256], buf2[256];
	int bl = sizeof(buf);
	
	len = lseek(fs, 0, SEEK_END);
	lseek(fs, 0, SEEK_SET);
	if (len > maxlen) {
		printf("file too big\n");
		return -1;
	}
	//printf("flash file len %u, compare to %08x in flash\n", len, addr);
	for (j = 0; j < len; j += bl, addr += bl) {
		if (len - j < bl)
			bl = len - j;
		flashread(ddf->fd, buf, addr, bl);
		rlen = read(fs, buf2, bl);
		if (rlen < 0 || rlen != bl) {
			printf("read error\n");
			return -1;
		}
			
		if (memcmp(buf, buf2, bl)) {
			printf("flash differs at %08x (offset %u)\n", addr, j);
			dump(buf, 32);
			dump(buf2, 32);
			return addr;
		}
	}
	//printf("flash same as file\n");
	return 0;
}


static int flash_detect(struct ddflash *ddf)
{
	uint8_t cmd = 0x9F;
	uint8_t id[3];
	
	int r = flashio(ddf->fd, &cmd, 1, id, 3);
	if (r < 0)
		return r;
	
	if (id[0] == 0xBF && id[1] == 0x25 && id[2] == 0x41) {
		r = SSTI_SST25VF016B; 
		//printf("Flash: SSTI  SST25VF016B 16 MBit\n");
		ddf->sector_size = 4096; 
		ddf->size = 0x200000; 
	} else if (id[0] == 0xBF && id[1] == 0x25 && id[2] == 0x4A) {
		r = SSTI_SST25VF032B; 
		//printf("Flash: SSTI  SST25VF032B 32 MBit\n");
		ddf->sector_size = 4096; 
		ddf->size = 0x400000; 
	} else if (id[0] == 0x1F && id[1] == 0x28) {
		r = ATMEL_AT45DB642D; 
		//printf("Flash: Atmel AT45DB642D  64 MBit\n");
		ddf->sector_size = 1024; 
		ddf->size = 0x800000; 
	} else {
		r = UNKNOWN_FLASH;
		//printf("Unknown Flash Flash ID = %02x %02x %02x\n", id[0], id[1], id[2]);
	}
	if (ddf->sector_size) {
		ddf->buffer = malloc(ddf->sector_size);
		//printf("allocated buffer %08x@%08x\n", ddf->sector_size, (uint32_t) ddf->buffer);  
	}
	return r;
}


int FlashWriteAtmel(int dev,uint32_t FlashOffset, uint8_t *Buffer,int BufferSize)
{
    int err = 0;
    int BlockErase = BufferSize >= 8192;
    int i;
    
    if (BlockErase) {
	    for (i = 0; i < BufferSize; i += 8192 ) {
		    uint8_t cmd[4];
		    if ((i & 0xFFFF) == 0 )
			    printf(" Erase    %08x\n",FlashOffset + i);
		    cmd[0] = 0x50; // Block Erase
		    cmd[1] = ( (( FlashOffset + i ) >> 16) & 0xFF );
		    cmd[2] = ( (( FlashOffset + i ) >>  8) & 0xFF );
		    cmd[3] = 0x00;
		    err = flashio(dev,cmd,4,NULL,0);
		    if (err < 0 ) break;
		    
		    while( 1 )
		    {
			    cmd[0] = 0xD7;  // Read Status register
			    err = flashio(dev,cmd,1,&cmd[0],1);
			    if (err < 0 ) break;
			    if ((cmd[0] & 0x80) == 0x80 ) break;
		    }
	    }
    }
    
    for (i = 0; i < BufferSize; i += 1024) {
        uint8_t cmd[4 + 1024];
        if ((i & 0xFFFF) == 0 )
        {
            printf(" Program  %08x\n",FlashOffset + i);
        }
        cmd[0] = 0x84; // Buffer 1
        cmd[1] = 0x00;
        cmd[2] = 0x00;
        cmd[3] = 0x00;
        memcpy(&cmd[4],&Buffer[i],1024);

        err = flashio(dev,cmd,4 + 1024,NULL,0);
        if (err < 0 ) break;

        cmd[0] = BlockErase ? 0x88 : 0x83; // Buffer to Main Memory (with Erase)
        cmd[1] = ( (( FlashOffset + i ) >> 16) & 0xFF );
        cmd[2] = ( (( FlashOffset + i ) >>  8) & 0xFF );
        cmd[3] = 0x00;

        err = flashio(dev,cmd,4,NULL,0);
        if (err < 0 ) break;

        while( 1 )
        {
		cmd[0] = 0xD7;  // Read Status register
		err = flashio(dev,cmd,1,&cmd[0],1);
            if (err < 0 ) break;
            if ((cmd[0] & 0x80) == 0x80 ) break;
        }
        if (err < 0 ) break;
    }
    return err;
}

int FlashWriteSSTI(int dev, uint32_t FlashOffset, uint8_t *Buffer, int BufferSize)
{
    int err = 0;
    uint8_t cmd[6];
    int i, j;

    // Must be multiple of sector size
    if ((BufferSize % 4096) != 0 ) 
	    return -1;   
    
    do {
	    cmd[0] = 0x50;  // EWSR
	    err = flashio(dev,cmd,1,NULL,0);
	    if (err < 0 ) 
		    break;

	    cmd[0] = 0x01;  // WRSR
	    cmd[1] = 0x00;  // BPx = 0, Unlock all blocks
	    err = flashio(dev,cmd,2,NULL,0);
	    if (err < 0 )
		    break;
	    
	    for (i = 0; i < BufferSize; i += 4096 ) {
		    if ((i & 0xFFFF) == 0 )
			    printf(" Erase    %08x\n",FlashOffset + i);
		    cmd[0] = 0x06;  // WREN
		    err = flashio(dev,cmd,1,NULL,0);
		    if (err < 0 )
			    break;
		    
		    cmd[0] = 0x20;  // Sector erase ( 4Kb)
		    cmd[1] = ( (( FlashOffset + i ) >> 16) & 0xFF );
		    cmd[2] = ( (( FlashOffset + i ) >>  8) & 0xFF );
		    cmd[3] = 0x00;
		    err = flashio(dev,cmd,4,NULL,0);
		    if (err < 0 )
			    break;
		    
		    while(1) {
			    cmd[0] = 0x05;  // RDRS
			    err = flashio(dev,cmd,1,&cmd[0],1);
			    if (err < 0 ) break;
			    if ((cmd[0] & 0x01) == 0 ) break;
		    }
		    if (err < 0 ) break;
	    }
	    if (err < 0 ) 
		    break;
	    for (j = BufferSize - 4096; j >= 0; j -= 4096 ) {
		    if ((j & 0xFFFF) == 0 )
			    printf(" Program  %08x\n",FlashOffset + j);
		    
		    for (i = 0; i < 4096; i += 2 ) {
			    if (i == 0 ) {
				    cmd[0] = 0x06;  // WREN
				    err = flashio(dev,cmd,1,NULL,0);
				    if (err < 0 ) 
					    break;
				    
				    cmd[0] = 0xAD;  // AAI
				    cmd[1] = ( (( FlashOffset + j ) >> 16) & 0xFF );
				    cmd[2] = ( (( FlashOffset + j ) >>  8) & 0xFF );
				    cmd[3] = 0x00;
				    cmd[4] = Buffer[j+i];
				    cmd[5] = Buffer[j+i+1];
				    err = flashio(dev,cmd,6,NULL,0);
			    } else {
				    cmd[0] = 0xAD;  // AAI
				    cmd[1] = Buffer[j+i];
				    cmd[2] = Buffer[j+i+1];
				    err = flashio(dev,cmd,3,NULL,0);
			    }
			    if (err < 0 ) 
				    break;
			    
			    while(1) {
				    cmd[0] = 0x05;  // RDRS
				    err = flashio(dev,cmd,1,&cmd[0],1);
				    if (err < 0 ) break;
				    if ((cmd[0] & 0x01) == 0 ) break;
			    }
			    if (err < 0 ) break;
		    }
		    if (err < 0 ) break;
		    
		    cmd[0] = 0x04;  // WDIS
		    err = flashio(dev,cmd,1,NULL,0);
		    if (err < 0 ) break;
		    
	    }
	    if (err < 0 ) break;
	    
	    cmd[0] = 0x50;  // EWSR
	    err = flashio(dev,cmd,1,NULL,0);
	    if (err < 0 ) break;
	    
	    cmd[0] = 0x01;  // WRSR
	    cmd[1] = 0x1C;  // BPx = 0, Lock all blocks
	    err = flashio(dev,cmd,2,NULL,0);
    } while(0);
    return err;
}

static int get_id(struct ddflash *ddf) {
	uint8_t id[4];

	if (ioctl(ddf->fd, IOCTL_DDB_ID, &ddf->id) < 0)
		return -1;
#if 0
	printf("%04x %04x %04x %04x %08x %08x\n",
	       ddf->id.vendor, ddf->id.device,
	       ddf->id.subvendor, ddf->id.subdevice,
	       ddf->id.hw, ddf->id.regmap);
#endif	
	if (ddf->id.device == 0x0011)
		ddf->type = 1;
	if (ddf->id.device == 0x0201)
		ddf->type = 2;
	if (ddf->id.device == 0x02)
		ddf->type = 3;
	if (ddf->id.device == 0x03)
		ddf->type = 0;
	if (ddf->id.device == 0x0300)
		ddf->type = 4;

	return 0;
}

static int update_image(struct ddflash *ddf, char *fn, 
			uint32_t adr, uint32_t len)
{
	int fs, res;

	fs = open(fn, O_RDONLY);
	if (fs < 0 ) {
		printf("File %s not found \n", fn);
		return -1;
	}
	res = flashcmp(ddf, fs, adr, len);
	if (res <= 0) 
		goto out;
	if (res > 0)
		res = flashwrite(ddf, fs, adr, len);
	if (res == 0)
		res = 1;
out:
	close(fs);
	return res;
}

static int update_flash(struct ddflash *ddf)
{
	char *fname;
	int res, stat = 0;

	switch (ddf->id.device) {
	case 0x300:
		//fname="/boot/DVBNetV1A_DD01_0300.bit";
		fname="/boot/fpga.img";
		break;
#if 0
	case 0x301:
		fname="/boot/DVBNetV1A_DD01_0301.bit";
		break;
#endif
	default:
		return 0;
	}
	if ((res = update_image(ddf, "/boot/bs.img", 0x4000, 0x1000)) == 1)
		stat |= 4;
	if ((res = update_image(ddf, "/boot/uboot.img", 0xb0000, 0xb0000)) == 1)
		stat |= 2;
	if ((res = update_image(ddf, fname, 0x10000, 0xa0000)) == 1)
		stat |= 1;
	return stat;
}

int main(int argc, char **argv)
{
	struct ddflash ddf;
	char ddbname[80];
	uint8_t *buffer = 0;
	uint32_t FlashOffset = 0x10000;
	int i, err, res;
	int ddbnum = 0;

	uint32_t svid, jump, flash;

	memset(&ddf, 0, sizeof(ddf));

        while (1) {
                int option_index = 0;
		int c;
                static struct option long_options[] = {
			{"svid", required_argument, NULL, 's'},
			{"help", no_argument , NULL, 'h'},
			{0, 0, 0, 0}
		};
                c = getopt_long(argc, argv, 
				"d:n:s:o:l:dfhj",
				long_options, &option_index);
		if (c==-1)
			break;

		switch (c) {
		case 's':
			svid = strtoul(optarg, NULL, 16);
			break;
		case 'o':
			FlashOffset = strtoul(optarg, NULL, 16);
			break;
		case 'n':
			ddbnum = strtol(optarg, NULL, 0);
			break;
		case 'j':
			jump = 1;
			break;
		case 'h':
		default:
			break;

		}
	}
	if (optind < argc) {
		printf("Warning: unused arguments\n");
	}
	sprintf(ddbname, "/dev/ddbridge/card%d", ddbnum);
	while ((ddf.fd = open(ddbname, O_RDWR)) < 0) {
		if (errno == EBUSY)
			usleep(100000);
		else {
			printf("Could not open device\n");
			return -1;
		}
	}
	flash = flash_detect(&ddf);
	get_id(&ddf);

	res = update_flash(&ddf);

	if (ddf.buffer)
		free(ddf.buffer);
	if (res < 0)
		return res;
	if (res & 1)
		reboot(60);
	return res;
}
