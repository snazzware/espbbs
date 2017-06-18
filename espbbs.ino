#include <ESP8266WiFi.h>          //https://github.com/esp8266/Arduino

#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>         //https://github.com/tzapu/WiFiManager
#include <stdarg.h>

#include "FS.h"

WiFiServer server(23);

#define MAX_INPUT 255
#define MAX_PATH 255

#define STAGE_INIT            0

// Login
#define BBS_LOGIN             1
#define BBS_LOGIN_USERNAME    1
#define BBS_LOGIN_PASSWORD    2

// Guest
#define BBS_GUEST             2

// Main Menu
#define BBS_MAIN              3
#define BBS_MAIN_SELECT       1

// Logout
#define BBS_LOGOUT            4

#define BBS_FILES             5
#define BBS_FILES_LIST        1
#define BBS_FILES_READ        2



char data[1500];
int ind = 0;

struct BBSClient {
  int action;
  int stage;
  bool inputting;
  char input[MAX_INPUT];
  int inputPos;
  char inputEcho;
  void *data;
};

struct BBSFileClient {
  char path[MAX_PATH];
  File *f;
};

#define MAX_CLIENTS 4

WiFiClient clients[MAX_CLIENTS];
BBSClient bbsclients[MAX_CLIENTS];

void setup() {
  Serial.begin(115200);
  
  SPIFFS.begin();
  
  WiFiManager wifiManager;
  wifiManager.autoConnect();
  
  Serial.println("Connection established");
  
  server.begin();
  Serial.println("Server started");
  Serial.setDebugOutput(true);
}

void sendTextFile(WiFiClient client, String filepath) {
  File f = SPIFFS.open(filepath, "r");
  if (!f) {
    client.write("file open failed\r\n");
  } else {
     size_t size = f.size();
    if ( size > 0 ) {
      char buf[32];
      char buf2[32];
      int len = 1;
      int ofs = 0;
     
      while (len) {
        ofs = 0;
         len = f.readBytes(buf, 32);
         for (int i=0;i<len;i++) {
          if (buf[i] == 10 || i+1 == len) {
             buf2[ofs++] = buf[i];
            if (buf[i] == 10) {
              buf2[ofs++] = '\r';
            }
            client.write((uint8_t *)buf2, ofs);
            ofs = 0;
          } else {
            buf2[ofs++] = buf[i];
          }
         }
      }
      
    }
    f.close();
  }
}

void cprintf(int clientNumber, char *fmt, ...) {
  char buf[255];
  va_list va;
  va_start (va, fmt);
  vsnprintf (buf, sizeof(buf), fmt, va);
  va_end (va);

  if (clients[clientNumber]) clients[clientNumber].write((uint8_t *)buf, strlen(buf));
}

void getInput(int clientNumber) {
  bbsclients[clientNumber].inputPos = 0;
  bbsclients[clientNumber].inputEcho = 0;
  bbsclients[clientNumber].input[0] = 0;
  bbsclients[clientNumber].inputting = true;
}

void getInput(int clientNumber, char echo) {
  getInput(clientNumber);
  bbsclients[clientNumber].inputEcho = echo;
}

void action (int clientNumber, int actionId, int stageId) {
  bbsclients[clientNumber].action = actionId;
  bbsclients[clientNumber].stage = stageId;
}

void action (int clientNumber, int actionId) {
  action(clientNumber, actionId, STAGE_INIT);
}

void loop() {
  char buf[128]; 
  char charIn;
  char buffer[16];
  char result;
  
  for (int i = 0; i < MAX_CLIENTS; i++) {
      if (clients[i]) {
         if (clients[i].status() == CLOSED) {
            clients[i].stop();
         } else {
            switch (bbsclients[i].action) {
              case BBS_LOGIN:
                switch (bbsclients[i].stage) {
                  case STAGE_INIT:
                    cprintf(i, "Node #%u - Username: ", i);
                    getInput(i);
                    action(i, BBS_LOGIN, BBS_LOGIN_USERNAME);
                  break;
                  case BBS_LOGIN_USERNAME:
                    if (!bbsclients[i].inputting && strlen(bbsclients[i].input)) {
                      if (strcmp(bbsclients[i].input, "guest") == 0) {
                        action(i, BBS_GUEST);
                      } else 
                      if (strcmp(bbsclients[i].input, "new") == 0) {
                        cprintf(i, "Sorry, new user signups have not been implemented. Try 'guest'.\r\n");
                        action(i, BBS_LOGIN);
                      } else {
                        cprintf(i, "Password: ");
                        getInput(i, '*');
                        action(i, BBS_LOGIN, BBS_LOGIN_PASSWORD);
                      }
                    }
                  break;
                  case BBS_LOGIN_PASSWORD:
                    if (!bbsclients[i].inputting) {
                      if (strlen(bbsclients[i].input)) {
                        cprintf(i, "Nope!\r\n");
                        action(i, BBS_LOGIN);
                      } else {
                        action(i, BBS_LOGIN);
                      }
                    }
                  break;
                }
              break;
              case BBS_GUEST:
                switch (bbsclients[i].stage) {
                  case STAGE_INIT:
                    cprintf(i, "Welcome, guest!\r\n");
                    action(i, BBS_MAIN, STAGE_INIT);
                  break;
                }
              break;
              case BBS_FILES:
                switch (bbsclients[i].stage) {
                  case STAGE_INIT: {
                      cprintf(i, "Browsing %s\r\n", ((BBSFileClient *)(bbsclients[i].data))->path);
                      cprintf(i, "B) Go Back\r\n");
                      
                      Dir dir = SPIFFS.openDir(((BBSFileClient *)(bbsclients[i].data))->path);
                      int idx = 1;
                      while (dir.next()) {
                        cprintf(i, "%u) %s\r\n", idx++, dir.fileName().c_str());
                      }
                      getInput(i);
            
                      action(i, BBS_FILES, BBS_FILES_LIST);
                  } break;
                   
                  case BBS_FILES_LIST: {
                    if (!bbsclients[i].inputting) {
                      if (strlen(bbsclients[i].input)>0) {
                        if (toupper(bbsclients[i].input[0]) == 'B') {   
                          action(i, BBS_MAIN);
                        } else {
                          int selection = atoi(bbsclients[i].input);
                          Dir dir = SPIFFS.openDir(((BBSFileClient *)(bbsclients[i].data))->path);
                          int readIdx = 1;
                          while (dir.next() && readIdx <= selection) {
                            if (readIdx == selection) {
                              sendTextFile(clients[i], dir.fileName());
                            }
                            readIdx++;
                          }
                        }
                      }
                      action(i, BBS_FILES);
                    }
                  } break;
                  
                }
              break;
              case BBS_MAIN:
                switch (bbsclients[i].stage) {
                  case STAGE_INIT:
                    cprintf(i, "Main Menu\r\n");
                    cprintf(i, "1) Log Out\r\n");
                    cprintf(i, "2) File Library\r\n");
                    getInput(i);
                    action(i, BBS_MAIN, BBS_MAIN_SELECT);
                  break;
                  case BBS_MAIN_SELECT:
                    if (!bbsclients[i].inputting) {
                      if (strlen(bbsclients[i].input)==1) {
                        switch (bbsclients[i].input[0]) {
                          case '1':
                            action(i, BBS_LOGOUT);
                          break;
                          case '2':
                            action(i, BBS_FILES);
                            if (bbsclients[i].data != NULL) free(bbsclients[i].data);
                            bbsclients[i].data = malloc(sizeof(struct BBSFileClient));
                            strcpy(((BBSFileClient *)(bbsclients[i].data))->path, "/files");
                          break;
                        }
                      }
                    }
                  break;
                }
              break;
              case BBS_LOGOUT:
                cprintf(i, "\r\nNO CARRIER\r\n\r\n");
                clients[i].stop();
              break;
            }

            // See if we have any data pending 
            result = clients[i].read();
            if (result != 255) {
              if (bbsclients[i].inputting) {
                if (result == 127 || result == 8) { // handle del / backspace
                  if (bbsclients[i].inputPos>0) {
                    result = 8; // convert DEL (127) to backspace (8)
                    bbsclients[i].input[bbsclients[i].inputPos] = 0;
                    bbsclients[i].inputPos--;
                  } else result = 0;
                } else 
                if (result == 13) {
                  if (bbsclients[i].inputPos > 0) {
                    bbsclients[i].inputting = false;
                    bbsclients[i].input[bbsclients[i].inputPos] = 0;
                    clients[i].write("\r\n");
                  }
                  clients[i].flush();
                } else {
                  if (bbsclients[i].inputPos+1 < MAX_INPUT) {
                    bbsclients[i].input[bbsclients[i].inputPos++] = result;
                    if (bbsclients[i].inputEcho) result = bbsclients[i].inputEcho;
                  } else result = 0;
                }
            
                // echo input to client
                if (result) clients[i].write((char)result);
              }
            }
         }
      }
      else {
        clients[i] = server.available();
        if (clients[i]) {
          // Send telnet configuration - we'll handle echoes, and we do not want to be in linemode.
          clients[i].write(255); // IAC
          clients[i].write(251); // WILL
          clients[i].write(1);   // ECHO
        
          clients[i].write(255); // IAC
          clients[i].write(251); // WILL
          clients[i].write(3);   // suppress go ahead
        
          clients[i].write(255); // IAC
          clients[i].write(252); // WONT
          clients[i].write(34);  // LINEMODE
        
          // reset bbs client data
          bbsclients[i].inputting = false;
          bbsclients[i].inputPos = 0;
          bbsclients[i].inputEcho = 0;
          bbsclients[i].input[0] = 0;
          bbsclients[i].action = BBS_LOGIN;
          bbsclients[i].stage = STAGE_INIT;
          bbsclients[i].data = (void *)NULL;

          // Send the title file
          sendTextFile(clients[i], "/title.ans");

          cprintf(i, "Log in as 'guest', or type new to create an account.\r\n");
          clients[i].flush(); // discard initial telnet data from client
        }
      }
   }

  // Catch extra callers
  WiFiClient client = server.available();
  if (client) {
    sprintf(buf, "All nodes are currently in use.\r\n");
    client.write((uint8_t *)buf, strlen(buf));
    client.stop();
  }

  // Let ESP8266 do some stuff
  yield();
}
