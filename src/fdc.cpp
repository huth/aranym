/* This file is part of STonX, the Atari ST Emulator for Unix/X
 * ============================================================
 * STonX is free software and comes with NO WARRANTY - read the file
 * COPYING for details
 *
 * The simple FDC emulation was derived from FAST's FDC code. FAST is
 * an Atari ST emulator written by Joachim Hoenig
 * (hoenig@informatik.uni-erlangen.de). Bugs are probably implemented by
 * me (nino@complang.tuwien.ac.at), so don't bother him with questions
 * regarding this code!
 *
 */

#include "sysdeps.h"
#include "hardware.h"
#include "cpu_emulation.h"
#include "parameters.h"

#define DEBUG 0
#include "debug.h"

/*	FIXME: O_SYNC not defined in mingw, is there a replacement value ?
 *	Does it make floppy working even without it ?
 */
#ifdef OS_mingw
#define O_SYNC 0
#endif

// parameters of default floppy (3.5" HD 1.44 MB)
#define SECSIZE		512
#define SPT			18
#define SIDES		2
#define TRACKS		80
#define SECTORS		(SPT * SIDES * TRACKS)

struct disk_geom
{
	int head, sides, tracks, sectors, secsize;
} disk[2];

int drive_fd[2] = { -1, -1 };

int dma_mode,dma_scr,dma_car,dma_sr;
int fdc_command,fdc_track,fdc_sector,fdc_data,fdc_status;

void remove_floppy()
{
	if (drive_fd[0] >= 0) {
		close(drive_fd[0]);
		drive_fd[0] = -1;
		D(bug("Floppy removed"));
	}
}

bool insert_floppy()
{
	remove_floppy();

	char *path = bx_options.floppy.path;

	if (strlen(path) == 0)	// is path to floppy defined?
		return false;

	int status = open(path, O_RDWR
#ifdef HAVE_O_FSYNC
			| O_FSYNC
#else
			| O_SYNC
#endif
#ifdef O_BINARY
			| O_BINARY
#endif
			);
	bool rw = true;
	if (status < 0) {
		status = open(path, O_RDONLY
#ifdef O_BINARY
					| O_BINARY
#endif
					);
		rw = false;
	}
	if (status < 0) {
		D(bug("Inserting of floppy failed."));
		return false;
	}

	D(bug("Floppy inserted %s", rw ? "read-write" : "read-only"));
	drive_fd[0] = status;
	return true;
}

void init_fdc(void)
{
	int i;
	unsigned char buf[512];

	insert_floppy();

	for (i=0; i<2; i++)
	{
		int fd=drive_fd[i];
		if (fd > 0)
		{
			// read bootsector
			lseek(fd, 0, SEEK_SET);
			read(fd, buf, 512);

			// detect floppy geometry from bootsector data
			int secsize=(buf[12]<<8)|buf[11];
			int sectors=(buf[20]<<8)|buf[19];
			int spt=buf[24];
			int sides=buf[26];
			bool valid = true;	// suppose the bootsector data is valid

			// check validity of data
			if (secsize <= 0 || sectors <= 0 || spt <=0 || sides <= 0) {
				// data is obviously invalid (probably unformatted disk)
				valid = false;
			}
			else {
				int tracks = sectors / spt / sides;
				// check if all sectors are on the tracks
				if ((sides * spt * tracks) != sectors)
					valid = false;
			}

			if (! valid) {
				// bootsector contains invalid data - use our default
				secsize = SECSIZE;
				spt = SPT;
				sides = SIDES;
				sectors = SECTORS;
			}

			// init struct data
			disk[i].head = 0;
			disk[i].sides = sides;
			disk[i].sectors = spt;
			disk[i].secsize = secsize;
			disk[i].tracks = (sectors / spt / sides);

			D(bug("FDC %c: %d/%d/%d %d bytes/sector",
				'A'+i,disk[i].sides,disk[i].tracks,disk[i].sectors,
				disk[i].secsize));
		}
	}
}

void fdc_exec_command (void)
{
	static int dir=1,motor=1;
	int sides,d;
	memptr address;
	long offset;
	long count;
	uint8 *buffer;

	address = (HWget_b(0xff8609)<<16)|(HWget_b(0xff860b)<<8)
			|HWget_b(0xff860d);
	buffer = Atari2HostAddr(address);
	int snd_porta = yamaha.getFloppyStat();
	D(bug("FDC DMA virtual address = %06x, physical = %08x, snd = %d", address, buffer, snd_porta));
	sides=(~snd_porta)&1;
	d=(~snd_porta)&6;
	switch(d)
	{
		case 2:
			d=0;
			break;
		case 4:
			d=1;
			break;
		case 6:
		case 0:
			d=-1;
			break;
	}
	D(bug("FDC command 0x%04x drive=%d",fdc_command,d));
	fdc_status=0;
	if (fdc_command < 0x80)
	{
		if (d>=0)
		{
			switch(fdc_command&0xf0)
			{
				case 0x00:
					D(bug("\tFDC RESTORE"));
					disk[d].head=0;
					fdc_track=0;
					break;
				case 0x10:
					D(bug("\tFDC SEEK to %d",fdc_data));
					disk[d].head += fdc_data-fdc_track;
					fdc_track=fdc_data;
					if (disk[d].head<0 || disk[d].head>=disk[d].tracks)
						disk[d].head=0;
					break;
				case 0x30:
					fdc_track+=dir;
				case 0x20:
					disk[d].head+=dir;
					break;
				case 0x50:
					fdc_track++;
				case 0x40:
					if (disk[d].head<disk[d].tracks)
						disk[d].head++;
					dir=1;
					break;
				case 0x70:
					fdc_track--;
				case 0x60:
					if (disk[d].head > 0)
						disk[d].head--;
					dir=-1;
					break;
			}
			if (disk[d].head==0)
				fdc_status |= 4;
			if (disk[d].head != fdc_track && (fdc_command & 4))
				fdc_status |= 0x10;
			if (motor)
				fdc_status |= 0x20;
		}
		else fdc_status |= 0x10;
	}
	else if ((fdc_command & 0xf0) == 0xd0)
	{
		if (fdc_command == 0xd8)
			mfp.setGPIPbit(0x20, 0);
		else if (fdc_command == 0xd0)
			mfp.setGPIPbit(0x20, 0x20);
	}
	else
	{
		if (d>=0)
		{
			offset=disk[d].secsize
				* (((disk[d].sectors*disk[d].sides*disk[d].head))
				+ (disk[d].sectors * sides) + (fdc_sector-1));
			switch(fdc_command & 0xf0)
			{
				case 0x80:
					D(bug("\tFDC READ SECTOR %d to 0x%06lx", (disk[d].secsize)?offset/disk[d].secsize:-1/*dma_scr*/,address));
					// special hack for remounting physical floppy on media change
					if (offset == 0) {
						D(bug("Trying to remount the floppy - media change requested?"));
						// reading boot sector might indicate media change
						init_fdc();
					}
					count=dma_scr*disk[d].secsize;
					if (lseek(drive_fd[d], offset, SEEK_SET)>=0)
					{
						if (count==read(drive_fd[d], buffer, count))
						{
							address += count;
							HWput_b(0xff8609, address>>16);
							HWput_b(0xff860b, address>>8);
							HWput_b(0xff860d, address);
							dma_scr=0;
							dma_sr=1;
							break;
						}
						else {
							D(bug("read(%d, %d, %d) didn't return correct len.", drive_fd[d], buffer, count));
						}
					}
					fdc_status |= 0x10;
					dma_sr=1;
					break;
				case 0x90:
					D(bug("\tFDC READ SECTOR M. %d to 0x%06lx", dma_scr,address));
					count=dma_scr*disk[d].secsize;
					if (lseek(drive_fd[d], offset, SEEK_SET)>=0)
					{
						if (count==read(drive_fd[d], buffer, count))
						{
							address += count;
							HWput_b(0xff8609, address>>16);
							HWput_b(0xff860b, address>>8);
							HWput_b(0xff860d, address);
							dma_scr=0;
							dma_sr=1;
							fdc_sector += dma_scr; /* *(512/disk[d].secsize);*/
							break;
						}
					}
					fdc_status |= 0x10;
					dma_sr=1;
					break;
				case 0xa0:
					count=dma_scr*disk[d].secsize;
					if (lseek(drive_fd[d], offset, SEEK_SET)>=0)
					{
						if (count==write(drive_fd[d], buffer, count))
						{
							address += count;
							HWput_b(0xff8609, address>>16);
							HWput_b(0xff860b, address>>8);
							HWput_b(0xff860d, address);
							dma_scr=0;
							dma_sr=1;
							break;
						}
					}
					fdc_status |= 0x10;
					dma_sr=1;
					break;
				case 0xb0:
					count=dma_scr*disk[d].secsize;
					if (lseek(drive_fd[d], offset, SEEK_SET)>=0)
					{
						if (count==write(drive_fd[d], buffer, count))
						{
							address += count;
							HWput_b(0xff8609, address>>16);
							HWput_b(0xff860b, address>>8);
							HWput_b(0xff860d, address);
							dma_scr=0;
							dma_sr=1;
							fdc_sector += dma_scr; /* *(512/disk[d].secsize);*/
							break;
						}
					}
					fdc_status |= 0x10;
					dma_sr=1;
					break;
				case 0xc0:
					fdc_status |= 0x10;
					break;
				case 0xe0:
					fdc_status |= 0x10;
					break;
				case 0xf0:
					fdc_status |= 0x10;
					break;
			}
			if (disk[d].head != fdc_track) fdc_status |= 0x10;
		}
		else fdc_status |= 0x10;
	}
	if (motor)
		fdc_status |= 0x80;
	if (!(fdc_status & 1))
		mfp.setGPIPbit(0x20, 0);
}


