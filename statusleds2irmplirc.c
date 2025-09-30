/*
 * StatusLeds2irmplirc.c: A plugin for the Video Disk Recorder
 *
 * See the README file for copyright information and how to reach the author.
 *
 */

#include <vdr/i18n.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#include <getopt.h>
#include <vdr/videodir.h>
#include <vdr/plugin.h>
#include <vdr/interface.h>
#include <vdr/status.h>
#include <vdr/osd.h>
#include <ctype.h>
#include <sys/kd.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#include <errno.h>
#include <fcntl.h>

extern char **environ;

static const char *VERSION        = "0.2";
static const char *DESCRIPTION    = tr("show vdr status on irmplirc");

enum access {
	ACC_GET,
	ACC_SET,
	ACC_RESET
};

enum command {
	CMD_EMIT,
	CMD_CAPS,
	CMD_HID_TEST,
	CMD_ALARM,
	CMD_MACRO,
	CMD_WAKE,
	CMD_REBOOT,
	CMD_EEPROM_RESET,
	CMD_EEPROM_COMMIT,
	CMD_EEPROM_GET_RAW,
	CMD_STATUSLED,
	CMD_NEOPIXEL,
};

enum status {
	STAT_CMD,
	STAT_SUCCESS,
	STAT_FAILURE
};

enum report_id {
	REPORT_ID_IR = 1,
	REPORT_ID_CONFIG_IN = 2,
	REPORT_ID_CONFIG_OUT = 3
};

static int stm32fd = -1;
uint8_t inBuf[64];
uint8_t outBuf[64];

static bool open_stm32(const char *devicename) {
	stm32fd = open(devicename, O_RDWR);
	if (stm32fd == -1) {
		dsyslog("statusleds2irmplirc: error opening stm32 device: %s\n",strerror(errno));
		printf("error opening stm32 device: %s\n",strerror(errno));
		return false;
	}
	//printf("opened stm32 device\n");
	return true;
}

static void read_stm32() {
	int retVal;
	retVal = read(stm32fd, inBuf, sizeof(inBuf));
	if (retVal < 0) {
		dsyslog("statusleds2irmplirc: read error\n");
		printf("read error\n");
        } /*else {
                printf("read %d bytes:\n\t", retVal);
                for (int i = 0; i < retVal; i++)
                        printf("%02hhx ", inBuf[i]);
                puts("\n");
        }*/
} 

static void write_stm32() {
	int retVal;
	retVal = write(stm32fd, outBuf, sizeof(outBuf));
	if (retVal < 0) {
		dsyslog("statusleds2irmplirc: write error\n");
		printf("write error\n");
        } /*else {
                printf("written %d bytes:\n\t", retVal);
                for (int i = 0; i < retVal; i++)
                        printf("%02hhx ", outBuf[i]);
                puts("\n");
        }*/
}

static void send_report(uint8_t led_state, const char *device) {
	open_stm32(device != NULL ? device : "/dev/irmp_stm32");
        outBuf[0] = REPORT_ID_CONFIG_OUT;
	outBuf[1] = STAT_CMD;

	    outBuf[2] = ACC_SET;
	    outBuf[3] = CMD_STATUSLED;
	    outBuf[4] = led_state;
	    write_stm32();
	    usleep(3000);
	    read_stm32();
	    while (inBuf[0] == REPORT_ID_IR)
		read_stm32();
	
	if (stm32fd >= 0) close(stm32fd);
}

class cStatusUpdate : public cThread, public cStatus {
private:
    bool active;
public:
    cStatusUpdate();
    ~cStatusUpdate();
#if VDRVERSNUM >= 10338
    virtual void Recording(const cDevice *Device, const char *Name, const char *FileName, bool On);
#else
    virtual void Recording(const cDevice *Device, const char *Name);
#endif
    void Stop();
protected:
    virtual void Action(void);
};

// Global variables that control the overall behaviour:
int iOnDuration = 1;
int iOffDuration = 10;
int iOnPauseDuration = 5; 
bool bPerRecordBlinking = false;
int iRecordings = 0;
bool bActive = false;
const char * irmplirc_device = NULL;
char State;

cStatusUpdate * oStatusUpdate = NULL;

class cPluginStatusLeds2irmplirc : public cPlugin {
private:
public:
  cPluginStatusLeds2irmplirc(void);
  virtual ~cPluginStatusLeds2irmplirc() override;
  virtual const char *Version(void) override { return VERSION; }
  virtual const char *Description(void) override { return tr(DESCRIPTION); }
  virtual const char *CommandLineHelp(void) override;
  virtual bool ProcessArgs(int argc, char *argv[]) override;
  virtual bool Start(void) override;
  virtual void Stop(void) override;
  virtual void Housekeeping(void) override;
  virtual const char *MainMenuEntry(void) override { return NULL; }
  virtual cMenuSetupPage *SetupMenu(void) override;
  virtual bool SetupParse(const char *Name, const char *Value) override;
  };

// --- cMenuSetupStatusLeds2irmplirc -------------------------------------------------------

class cMenuSetupStatusLeds2irmplirc : public cMenuSetupPage {
private:
  int iNewOnDuration;
  int iNewOffDuration;
  int iNewOnPauseDuration;
  int bNewPerRecordBlinking;
protected:
  virtual void Store(void);
  void Set(void);
  void Save();
  eOSState ProcessKey(eKeys Key);
public:
  cMenuSetupStatusLeds2irmplirc(void);
  };

cMenuSetupStatusLeds2irmplirc::cMenuSetupStatusLeds2irmplirc(void)
{
  iNewOnDuration = iOnDuration;
  iNewOffDuration = iOffDuration;
  iNewOnPauseDuration = iOnPauseDuration;
  bNewPerRecordBlinking = bPerRecordBlinking;

  Set();
}

void cMenuSetupStatusLeds2irmplirc::Set(void)
{
  int current = Current();
  Clear();

  Add(new cMenuEditBoolItem( tr("Setup.StatusLeds2irmplirc$One blink per recording"), &bNewPerRecordBlinking));

  // Add ioctl() options
  Add(new cMenuEditIntItem( tr("Setup.StatusLeds2irmplirc$On time (100ms)"), &iNewOnDuration, 1, 99));
  Add(new cMenuEditIntItem( tr("Setup.StatusLeds2irmplirc$On pause time (100ms)"), &iNewOnPauseDuration, 1, 99));
  Add(new cMenuEditIntItem( tr("Setup.StatusLeds2irmplirc$Off time (100ms)"), &iNewOffDuration, 1, 99));

  SetCurrent(Get(current));
}

eOSState cMenuSetupStatusLeds2irmplirc::ProcessKey(eKeys Key)
{
  eOSState state = cMenuSetupPage::ProcessKey(Key);

  return state;
}

void cMenuSetupStatusLeds2irmplirc::Save(void)
{
  iOnDuration = iNewOnDuration;
  iOffDuration = iNewOffDuration;
  iOnPauseDuration = iNewOnPauseDuration;
  bPerRecordBlinking = bNewPerRecordBlinking;
}

void cMenuSetupStatusLeds2irmplirc::Store(void)
{
  Save();

  SetupStore("OnDuration", iOnDuration);
  SetupStore("OffDuration", iOffDuration);
  SetupStore("OnPauseDuration", iOnPauseDuration);
  SetupStore("PerRecordBlinking", bPerRecordBlinking);
}

// --- cPluginStatusLeds2irmplirc ----------------------------------------------------------

cPluginStatusLeds2irmplirc::cPluginStatusLeds2irmplirc(void)
{
  // Initialize any member variables here.
  // DON'T DO ANYTHING ELSE THAT MAY HAVE SIDE EFFECTS, REQUIRE GLOBAL
  // VDR OBJECTS TO EXIST OR PRODUCE ANY OUTPUT!
}

cPluginStatusLeds2irmplirc::~cPluginStatusLeds2irmplirc()
{
  // Clean up after yourself!
  if (oStatusUpdate)
  {
    delete oStatusUpdate;
    oStatusUpdate = NULL;
  }
}

const char *cPluginStatusLeds2irmplirc::CommandLineHelp(void)
{
  // Return a string that describes all known command line options.
  return

"  -p, --perrecordblinking                    LED blinks one times per recording\n"
"  -d [on[,off[,pause]]],                     LED blinking timing\n"
"     --duration[=On-Time[,Off-Time[,On-Pause-Time]]]\n"
"  -i irmplirc_device, --irmplirc_device=irmplirc_device  irmplirc_device\n"
;
}

bool cPluginStatusLeds2irmplirc::ProcessArgs(int argc, char *argv[])
{
  // Implement command line argument processing here if applicable.
  static struct option long_options[] = {
       { "duration",		optional_argument,	NULL, 'd' },
       { "perrecordblinking",	no_argument,		NULL, 'p' },
       { "irmplirc_device",	optional_argument,	NULL, 'i' },
       { NULL,			no_argument,		NULL, 0 }
     };

  int c;
  while ((c = getopt_long(argc, argv, "d:pi", long_options, NULL)) != -1) {
        switch (c) {
          case 'd':
            iOnDuration = 1;
            iOffDuration = 10;
            iOnPauseDuration = 5;
            if (optarg && *optarg)
              sscanf(optarg, "%d,%d,%d", &iOnDuration, &iOffDuration, &iOnPauseDuration);
            break;
          case 'p': 
            bPerRecordBlinking = true;
            break;
          case 'i':
            irmplirc_device = optarg;
            break;
          default:
            return false;
          }
        }
  return true;
}

cStatusUpdate::cStatusUpdate()
{
}

cStatusUpdate::~cStatusUpdate()
{
  if (oStatusUpdate)
  {
    // Perform any cleanup or other regular tasks.
    bActive = false;

    // Stop threads
    oStatusUpdate->Stop();
  }
}

void cStatusUpdate::Action(void)
{
    dsyslog("statusleds2irmplirc: Thread started (pid=%d)", getpid());

    bool blinking = false;
    // turn the LED's on at start of VDR
    send_report(1 ,irmplirc_device);
    dsyslog("statusleds2irmplirc: turned LED on at start");

    for(bActive = true; bActive;) {
        if (iRecordings > 0) {
          //  let the LED's blink, if there's a recording
          if(!blinking) {
            blinking = true;
          }
          for(int i = 0; i < (bPerRecordBlinking ? iRecordings : 1) && bActive; i++) {
            send_report(1 ,irmplirc_device);
            usleep(iOnDuration * 100000);

            send_report(0 ,irmplirc_device);
            usleep(iOnPauseDuration * 100000);
          }
          usleep(iOffDuration * 100000);
        } else {
          //  turn the LED's on, if there's no recording
          if(blinking) {
            send_report(1 ,irmplirc_device);
            blinking = false;
          }
          sleep(1);
        }
    }
    dsyslog("statusleds2irmplirc: Thread ended (pid=%d)", getpid());
}

bool cPluginStatusLeds2irmplirc::Start(void)
{
    // Start any background activities the plugin shall perform.
    oStatusUpdate = new cStatusUpdate;
    oStatusUpdate->Start();

    return true;
}

void cPluginStatusLeds2irmplirc::Stop(void)
{
  // turn the LED's off, when VDR stops
  send_report(0 ,irmplirc_device);
  dsyslog("statusleds2irmplirc: stopped (pid=%d)", getpid());
}

void cPluginStatusLeds2irmplirc::Housekeeping(void)
{
}

cMenuSetupPage *cPluginStatusLeds2irmplirc::SetupMenu(void)
{
  // Return a setup menu in case the plugin supports one.
  return new cMenuSetupStatusLeds2irmplirc;
}

bool cPluginStatusLeds2irmplirc::SetupParse(const char *Name, const char *Value)
{
  // Parse your own setup parameters and store their values.

  if (!strcasecmp(Name, "OnDuration"))
  {
    iOnDuration = atoi(Value);
    if (iOnDuration < 0 || iOnDuration > 99)
      iOnDuration = 1;
  }
  else if (!strcasecmp(Name, "OffDuration")) 
  {
    iOffDuration = atoi(Value);
    if (iOffDuration < 0 || iOffDuration > 99)
      iOffDuration = 10;
  }
  else if (!strcasecmp(Name, "OnPauseDuration"))
  {
    iOnPauseDuration = atoi(Value);
    if (iOnPauseDuration < 0 || iOnPauseDuration > 99)
      iOnPauseDuration = 5;
  }
  else if (!strcasecmp(Name, "PerRecordBlinking"))
  {
    bPerRecordBlinking = atoi(Value);
  }

  else
    return false;

  return true;
}

void cStatusUpdate::Stop()
{
  oStatusUpdate->Cancel(((iOnDuration + iOnPauseDuration) * (bPerRecordBlinking ? iRecordings : 1) + iOffDuration) * 10 + 1);
}

#if VDRVERSNUM >= 10338
void cStatusUpdate::Recording(const cDevice *Device, const char *Name, const char *FileName, bool On)
{
  if (On)
#else
void cStatusUpdate::Recording(const cDevice *Device, const char *Name)
{
  if (Name)
#endif
    iRecordings++;
  else
    iRecordings--;
}

VDRPLUGINCREATOR(cPluginStatusLeds2irmplirc); // Don't touch this!
