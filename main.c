//---------------------------------------------------------------------------------------------
//
// Copyright (c) 2017, fmad engineering llc 
//
// Fast PCAP de-encapsulation tool. Automatically de-encapsulate PCAPs with some basic filtering
//
//---------------------------------------------------------------------------------------------

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/ioctl.h>
#include <linux/sched.h>

#include "fTypes.h"
#include "fNetwork.h"

//-------------------------------------------------------------------------------------------------

double TSC2Nano = 0;
bool g_Verbose 		= false;			// verbose output
bool g_Dump 		= false;			// dump every packet

u16 DeEncapsulate(	u64 TS,
					fEther_t** pEther, 

					u8** pPayload, 
					u32* pPayloadLength,

					u32* MetaPort, 
					u64* MetaTS, 
					u32* MetaFCS);


void ERSPAN3Open(void);
void ERSPAN3Close(void);

//-------------------------------------------------------------------------------------------------

static void Help(void)
{
	printf("pcap_decap \n");
	printf("\n");
	printf("Command works entirely based linux input / ouput pipes.\n"); 
	printf("For example:\n");
	printf("$ cat erspan.pcap | pcap_decap > output.pcap\n");
	printf("\n");
	printf("Options:\n");
	printf("-v                 : verbose output\n");
	printf("-vv                : dump every packet\n");
	printf("\n");
}

//-------------------------------------------------------------------------------------------------
// prints number comma seperated
u8* PrettyNumber(u64 num)
{
	// nasty ... but dont have t care about collecting
	static u8 out[128][128];
	static u32 out_pos = 0;

	u8* Buffer = out[out_pos++];
	out_pos = out_pos & 127;	

	u8 Value[128];
	sprintf(Value, "%lli", num);

	memset(Buffer, 0x20, 128);
	int pos = 0;
	for (int i=strlen(Value)-1; i >= 0; i--)
	{
		Buffer[15 - pos] = Value[i]; 
		pos++;
		if ((i != 0) && ((i % 3) == 0))
		{
			Buffer[15 - pos] = ',';
			pos++;
		}
	}
	Buffer[16] = 0;

	return Buffer;
}

//-------------------------------------------------------------------------------------------------

int main(int argc, char* argv[])
{
	for (int i=1; i < argc; i++)
	{
		if (strcmp(argv[i], "--help") == 0)
		{
			Help();
			return 0;
		}
		else if (strcmp(argv[i], "-v") == 0)
		{
			fprintf(stderr, "Verbose Output\n");
			g_Verbose = true;
		}
		else if (strcmp(argv[i], "-vv") == 0)
		{
			fprintf(stderr, "Dump Output\n");
			g_Dump = true;
		}
	}
	FILE* InFile  = stdin;
	FILE* OutFile = stdout;

	// write output pcap header 
	PCAPHeader_t		Header;
	Header.Magic		= PCAPHEADER_MAGIC_NANO;
	Header.Major		= PCAPHEADER_MAJOR;
	Header.Minor		= PCAPHEADER_MINOR;
	Header.TimeZone		= 0; 
	Header.SigFlag		= 0; 
	Header.SnapLen		= 65535; 
	Header.Link			= PCAPHEADER_LINK_ETHERNET; 
	if (fwrite(&Header, sizeof(Header), 1, OutFile) != 1)
	{
		fprintf(stderr, "Failed to write header to output\n");
		return 0;
	}

	// read the header
	PCAPHeader_t		InputHeader;
	if (fread(&InputHeader,  sizeof(InputHeader), 1, InFile) != 1)
	{
		fprintf(stderr, "Input pcap failed to read\n");
		return 0;
	}

	// what kind of pcap
	u64 TimeScale = 0; 
	if (InputHeader.Magic == PCAPHEADER_MAGIC_NANO)
	{
		fprintf(stderr, "Found PCAP Nano\n");
		TimeScale = 1;
	}
	if (InputHeader.Magic == PCAPHEADER_MAGIC_USEC)
	{
		fprintf(stderr, "Found PCAP USec\n");
		TimeScale = 1000;
	}
	if (TimeScale == 0)
	{
		fprintf(stderr, "Invalid input PCAP magic. Found: %08x Expect %08x\n",
				InputHeader.Magic, PCAPHEADER_MAGIC_NANO);
		return 0;
	}

	u64 TotalBytes 		= 0;
	u64 TotalPacket		= 0;
	u64 T0 				= rdtsc();

	u8* PktInput		= malloc(32*1024);
	u8* PktOutput		= malloc(32*1024);

	memset(PktInput, 0, 16*1024);

	PCAPPacket_t 	HeaderInput;	
	PCAPPacket_t 	HeaderOutput;	

	// init protocol stats
	ERSPAN3Open();

	while (true)
	{
		// read pcap header
		if (fread(&HeaderInput, sizeof(HeaderInput), 1, InFile) != 1)
		{
			break;
		}

		if (fread(PktInput, HeaderInput.LengthCapture, 1, InFile) != 1)
		{
			break;
		}
		//fprintf(stderr, "size: %i\n", HeaderInput.LengthCapture);

		// PCAP timestamp
		u64 TS = (u64)HeaderInput.Sec * 1000000000ULL + (u64)HeaderInput.NSec * TimeScale; 


		fEther_t* Ether = (fEther_t*)PktInput;

		// assume payload has no de-encapsulation
		u8* Payload 		= PktInput + sizeof(fEther_t);
		u32 PayloadLength	= HeaderInput.LengthCapture - sizeof(fEther_t);

		u32 MetaPort 	= 0;
		u64 MetaTS 		= TS;		// default assume pcap TS
		u32 MetaFCS 	= 0;

		u32 EtherProto = DeEncapsulate(	TS,
										&Ether, 
										&Payload, 
										&PayloadLength,
										&MetaPort, 
										&MetaTS, 
										&MetaFCS
									  );

		// update capture length based on stripped packet 
		HeaderOutput.LengthCapture	= PayloadLength + sizeof(fEther_t); 
		HeaderOutput.LengthWire		= HeaderInput.LengthWire; 

		// re-write timestamp, decoder may have updated 
		HeaderOutput.Sec			= MetaTS / 1000000000ULL; 
		HeaderOutput.NSec			= MetaTS % 1000000000ULL; 

		fwrite(&HeaderOutput, sizeof(HeaderOutput), 1, OutFile);

		// write ether header 
		Ether->Proto				= swap16(EtherProto);
		fwrite(Ether, sizeof(fEther_t), 1, OutFile);

		// write payload
		fwrite(Payload, HeaderOutput.LengthCapture - sizeof(fEther_t), 1, OutFile);
	}

	// print protocol stats 
	ERSPAN3Close();

	return 0;
}