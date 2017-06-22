#include <ESP8266WiFi.h>          //https://github.com/esp8266/Arduino

#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>         //https://github.com/tzapu/WiFiManager
#include <stdarg.h>

#include "FS.h"

WiFiServer server(23);

#define MAX_INPUT 255
#define MAX_PATH 255

// Number of bytes that we buffer at a time while streaming files to clients
#define FILE_STREAM_BUFFER_SIZE 64

// Max number of lines to send client at once, when paging textfiles
#define LINES_PER_PAGE        25

// This pin will be set to high when a client connects
#define CONNECT_INDICATOR_PIN D1
// Duration in millis for how long we will leave the connect indicator pin high
#define CONNECT_INDICATOR_DURATION 3000

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

// File Area
#define BBS_FILES             5
#define BBS_FILES_LIST        1
#define BBS_FILES_READ        2

// Users
#define BBS_USER              6
#define BBS_USER_NEW_NAME     1
#define BBS_USER_NEW_PASSWORD 2
#define BBS_USER_NEW_CONFIRM  3
#define BBS_USER_NEW_FINALIZE 4
#define BBS_USER_WHOS_ONLINE  10

char data[1500];
int ind = 0;

struct BBSUser {
  char username[64]; // alphanumeric, lowercase
  char password[64]; // Plaintext - not too concerned about actual security here, folks!
  char twitterHandle[64];
  char githubHandle[64];
};

struct BBSClient {
  int action;
  int stage;
  bool inputting;
  char input[MAX_INPUT];
  int inputPos;
  char inputEcho;
  bool inputSingle;
  void *data;
  BBSUser user;
};

struct BBSFileClient {
  char path[MAX_PATH];
  File f;
  char buffer[FILE_STREAM_BUFFER_SIZE];
  int bufferPosition;
  int bufferUsed;
  int lineCount;
  bool nonstop;
};

struct BBSInfo {
  int callersTotal;
  int callersToday;
};

#define MAX_CLIENTS 4

WiFiClient clients[MAX_CLIENTS];
BBSClient bbsclients[MAX_CLIENTS];
BBSInfo bbsInfo;
long connectIndicatorMillis;

void persistBBSInfo() {
  File f = SPIFFS.open("/bbsinfo.dat", "w+");
  if (f) {
    f.write((unsigned char *)&bbsInfo, sizeof(bbsInfo));
    f.close();
  }
}

void setup() {
  Serial.begin(115200);
  
  SPIFFS.begin();

  File f = SPIFFS.open("/bbsinfo.dat", "r");
  if (f) {
    f.readBytes((char *)&bbsInfo, sizeof(bbsInfo));
    f.close();
  }

  pinMode(CONNECT_INDICATOR_PIN, OUTPUT);
  
  WiFiManager wifiManager;
  //wifiManager.resetSettings();
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

void clearEntireLine(int clientNumber) {
  cprintf(clientNumber, "\33[2K\r");
}

void pageTextFile(int clientNumber) {
  
  char buf2[FILE_STREAM_BUFFER_SIZE];
  int ofs = 0;
  BBSFileClient *client = ((BBSFileClient *)(bbsclients[clientNumber].data));
  
  // If we have processed all of our buffered data, read in some more
  if (client->bufferPosition >= client->bufferUsed) {
    client->bufferPosition = 0;
    client->bufferUsed = client->f.readBytes(client->buffer, FILE_STREAM_BUFFER_SIZE);
  }
  
   for (int i=client->bufferPosition;i<client->bufferUsed;i++) {
    if (client->buffer[i] == 10) {
      buf2[ofs++] = client->buffer[i];
      if (client->buffer[i] == 10) {
        buf2[ofs++] = '\r';
        client->lineCount++;
      }
      clients[clientNumber].write((uint8_t *)buf2, ofs);
      ofs = 0;

      if (client->lineCount >= LINES_PER_PAGE) {
        if (!client->nonstop) {
          cprintf(clientNumber, "ESC=Cancel, Space=Continue, Enter=Nonstop");
          getInputSingle(clientNumber);
          client->lineCount = 0;
        }
        client->bufferPosition = i+1;
        /*if (i+1 != len) {
          memcpy(((BBSFileClient *)(bbsclients[clientNumber].data))->overflow, buf+i, len - i);
          ((BBSFileClient *)(bbsclients[clientNumber].data))->overflowSize = len - i;
        }*/
        return;
      }
    } else {
      buf2[ofs++] = client->buffer[i];
    }
   }
   // write what we've buffered to the client
   if (ofs > 0) {
    clients[clientNumber].write((uint8_t *)buf2, ofs);
   }
   
   // Record final buffer position
   client->bufferPosition = client->bufferUsed;

  // If we did not obtain any more data this pass, close file
  if (!client->bufferUsed) {
    client->f.close();
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
  bbsclients[clientNumber].inputSingle = false;
  bbsclients[clientNumber].inputting = true;
}

void getInput(int clientNumber, char echo) {
  getInput(clientNumber);
  bbsclients[clientNumber].inputEcho = echo;
}

void getInputSingle(int clientNumber) {
  getInput(clientNumber);
  bbsclients[clientNumber].inputSingle = true;
}

void action (int clientNumber, int actionId, int stageId) {
  bbsclients[clientNumber].action = actionId;
  bbsclients[clientNumber].stage = stageId;

  // reset input when we change actions
  bbsclients[clientNumber].input[0] = 0;
}

void action (int clientNumber, int actionId) {
  action(clientNumber, actionId, STAGE_INIT);
}

void handleBBSUser(int clientNumber) {
  File f; // Declared here since compiler complains about case crossing otherwise. :P
  
  switch (bbsclients[clientNumber].stage) {
    case STAGE_INIT:
      action(clientNumber, BBS_USER, BBS_USER_NEW_NAME);
    break;
    case BBS_USER_NEW_NAME:
      if (!bbsclients[clientNumber].inputting) {
        if (strlen(bbsclients[clientNumber].input)>0) {
          bool valid = true;
          for (int j=0;j<strlen(bbsclients[clientNumber].input) && valid;j++) {
            if (!isalnum(bbsclients[clientNumber].input[j])) valid = false;
            else bbsclients[clientNumber].input[j] = tolower(bbsclients[clientNumber].input[j]);
          }
          if (!valid) {
            cprintf(clientNumber, "Non-alphanumeric character detected.\r\n");
          } else {
            char testPath[255];
            sprintf(testPath, "/users/%s.dat", bbsclients[clientNumber].input);
            if (SPIFFS.exists(testPath)) {
              cprintf(clientNumber, "Username '%s' is already taken.", bbsclients[clientNumber].input);
              valid = false;
            } else {
              if (strcmp(bbsclients[clientNumber].input, "new") == 0 ||
                  strcmp(bbsclients[clientNumber].input, "guest") == 0 ||
                  strcmp(bbsclients[clientNumber].input, "sysop") == 0) {
                    cprintf(clientNumber, "Username '%s' is not allowed.\r\n", bbsclients[clientNumber].input);
                    valid = false;
                  }
            }
          }

          if (valid) {
            strcpy(bbsclients[clientNumber].user.username, bbsclients[clientNumber].input);
            action(clientNumber, BBS_USER, BBS_USER_NEW_PASSWORD);
          }
        } else {
          cprintf(clientNumber, "Enter your desired username (lowercase, alphanumeric only): ");
          getInput(clientNumber);
        }
      }
    break;
    case BBS_USER_NEW_PASSWORD:
      if (!bbsclients[clientNumber].inputting) {
        if (strlen(bbsclients[clientNumber].input)>0) {
          strcpy(bbsclients[clientNumber].user.password, bbsclients[clientNumber].input);
          action(clientNumber, BBS_USER, BBS_USER_NEW_CONFIRM);
        } else {
          cprintf(clientNumber, "Enter password (64 character maximum): ");
          getInput(clientNumber);
        }
      }
    break;
    case BBS_USER_NEW_CONFIRM:
      if (!bbsclients[clientNumber].inputting) {
        if (strlen(bbsclients[clientNumber].input)>0) {
          if (strcmp(bbsclients[clientNumber].user.password, bbsclients[clientNumber].input) != 0) {
            cprintf(clientNumber, "Passwords do not match.\r\n");
            action(clientNumber, BBS_USER, BBS_USER_NEW_PASSWORD);
          } else {
            action(clientNumber, BBS_USER, BBS_USER_NEW_FINALIZE);
          }
        } else {
          cprintf(clientNumber, "Re-enter password (64 character maximum): ");
          getInput(clientNumber);
        }
      }
    break;
    case BBS_USER_NEW_FINALIZE:
      char userPath[255];
      sprintf(userPath, "/users/%s.dat", bbsclients[clientNumber].user.username);
      f = SPIFFS.open(userPath, "w+");
      if (f) {
        f.write((unsigned char *)&(bbsclients[clientNumber].user), sizeof(bbsclients[clientNumber].user));
        f.close();
        action(clientNumber, BBS_MAIN);
      } else {
        cprintf(clientNumber, "Fatal Error - Unable to create user file.");
        action(clientNumber, BBS_LOGIN);
      }
    break;
    case BBS_USER_WHOS_ONLINE:
      for (int j=0;j<MAX_CLIENTS;j++) {
        if (clients[j]) {
          cprintf(clientNumber, "Node #%u: %s\r\n", j, bbsclients[j].user.username);
        } else {
          cprintf(clientNumber, "Node #%u: Open\r\n");
        }
        action(clientNumber, BBS_MAIN);
      }
    break;
  }
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
                    action(i, BBS_LOGIN, BBS_LOGIN_USERNAME);
                  break;
                  case BBS_LOGIN_USERNAME:
                    if (!bbsclients[i].inputting) {
                      if (strlen(bbsclients[i].input)) {
                        if (strcmp(bbsclients[i].input, "guest") == 0) {
                          strcpy(bbsclients[i].user.username, "guest");
                          action(i, BBS_GUEST);
                        } else 
                        if (strcmp(bbsclients[i].input, "new") == 0) {
                          strcpy(bbsclients[i].user.username, "new user");
                          action(i, BBS_USER);
                        } else {
                          bool valid = true;
  
                          for (int j=0;j<strlen(bbsclients[i].input) && valid;j++) {
                            if (!isalnum(bbsclients[i].input[j])) valid = false;
                            else bbsclients[i].input[j] = tolower(bbsclients[i].input[j]);
                          }
  
                          if (valid) {
                            strcpy(bbsclients[i].user.username, bbsclients[i].input);
                            
                            action(i, BBS_LOGIN, BBS_LOGIN_PASSWORD);
                          } else {
                            cprintf(i, "Invalid username. Usernames are lowercase, alphanumeric only.");
                          }
                        }
                      } else {
                        cprintf(i, "Node #%u - Username: ", i);
                        getInput(i);
                      }
                    }
                  break;
                  case BBS_LOGIN_PASSWORD:
                    if (!bbsclients[i].inputting) {
                      if (strlen(bbsclients[i].input)) {
                        bool valid = false;
                        char testPath[255];
                        sprintf(testPath, "/users/%s.dat", bbsclients[i].user.username);
                        if (SPIFFS.exists(testPath)) {
                          File f = SPIFFS.open(testPath, "r");
                          if (f) {
                            f.readBytes((char *)&bbsclients[i].user, sizeof(bbsclients[i].user));
                            f.close();
                            if (strcmp(bbsclients[i].user.password, bbsclients[i].input) == 0) valid = true;
                          }
                        }
                        if (!valid) {
                          cprintf(i, "Username or password incorrect (should be %s).\r\n", bbsclients[i].user.password);
                        }
                        action(i, valid ? BBS_MAIN : BBS_LOGIN);
                      } else {
                        cprintf(i, "Password: ");
                        getInput(i, '*');
                      }
                    }
                  break;
                }
              break;
              case BBS_USER:
                handleBBSUser(i);
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
                      if (toupper(bbsclients[i].input[0]) == 'B') {   
                        action(i, BBS_MAIN);
                      } else {
                        int selection = atoi(bbsclients[i].input);
                        Dir dir = SPIFFS.openDir(((BBSFileClient *)(bbsclients[i].data))->path);
                        if (selection > 0 && selection < 255) {
                          int readIdx = 1;
                          while (dir.next() && readIdx <= selection) {
                            if (readIdx == selection) {
                              ((BBSFileClient *)(bbsclients[i].data))->f = SPIFFS.open(dir.fileName(), "r");
                              ((BBSFileClient *)(bbsclients[i].data))->bufferPosition = 0;
                              ((BBSFileClient *)(bbsclients[i].data))->bufferUsed = 0;
                              ((BBSFileClient *)(bbsclients[i].data))->lineCount = 0;
                              ((BBSFileClient *)(bbsclients[i].data))->nonstop = false;
                              bbsclients[i].input[0] = 0; // clear input
                              pageTextFile(i);
                              action(i, BBS_FILES, BBS_FILES_READ);
                            }
                            readIdx++;
                          }
                        } else { // selection out of bounds
                          action(i, BBS_FILES);
                        }
                      }
                    }
                  } break;
                  case BBS_FILES_READ: {
                    if (!((BBSFileClient *)(bbsclients[i].data))->f) {
                      action(i, BBS_FILES);
                    } else {
                      if (!bbsclients[i].inputting || ((BBSFileClient *)(bbsclients[i].data))->nonstop) {
                        
                        if (bbsclients[i].input[0] > 0) {
                          clearEntireLine(i); // clear the prompt
                        }
                        
                        if (bbsclients[i].input[0] == 13) { // ENTER
                          ((BBSFileClient *)(bbsclients[i].data))->nonstop = true;
                          pageTextFile(i);
                        } else
                        if (bbsclients[i].input[0] == 27) { // ESC
                          action(i, BBS_FILES);
                        } else {
                          pageTextFile(i);
                        }

                        // clear the input
                        bbsclients[i].input[0] = 0;
                        
                      }
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
                    cprintf(i, "3) Who's Online\r\n");
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
                          case '3':
                            action(i, BBS_USER, BBS_USER_WHOS_ONLINE);
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
                if (bbsclients[i].inputSingle) {
                  bbsclients[i].input[bbsclients[i].inputPos++] = result;
                  bbsclients[i].inputting = false;
                  bbsclients[i].input[bbsclients[i].inputPos] = 0;
                  result = 0; // suppress echo
                } else
                if (result == 127 || result == 8) { // handle del / backspace
                  if (bbsclients[i].inputPos>0) {
                    result = 8; // convert DEL (127) to backspace (8)
                    bbsclients[i].input[bbsclients[i].inputPos] = 0;
                    bbsclients[i].inputPos--;
                    // backspace
                    clients[i].write((char)result);
                    // white space
                    clients[i].write(' ');
                    // final backspace will occur w/ normal echo code, below
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
          digitalWrite(CONNECT_INDICATOR_PIN, HIGH);
          connectIndicatorMillis = millis();
          
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
          bbsclients[i].inputSingle = false;
          bbsclients[i].input[0] = 0;
          bbsclients[i].action = BBS_LOGIN;
          bbsclients[i].stage = STAGE_INIT;
          bbsclients[i].data = (void *)NULL;
          strcpy(bbsclients[i].user.username, "Logging in...");

          // Send the title file
          sendTextFile(clients[i], "/title.ans");

          cprintf(i, "You are caller #%u today.\r\n", bbsInfo.callersToday, bbsInfo.callersTotal);
          cprintf(i, "Log in as 'guest', or type new to create an account.\r\n");
          clients[i].flush(); // discard initial telnet data from client

          // update and save stats
          bbsInfo.callersTotal++;
          bbsInfo.callersToday++;
          persistBBSInfo();
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

  // reset connect indicator pin
  if (connectIndicatorMillis + CONNECT_INDICATOR_DURATION <= millis()) {
    digitalWrite(CONNECT_INDICATOR_PIN, LOW);
  }

  // Let ESP8266 do some stuff
  yield();
}
