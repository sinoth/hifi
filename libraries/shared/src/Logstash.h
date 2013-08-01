//
//  Logstash.h
//  hifi
//
//  Created by Stephen Birarda on 6/11/13.
//  Copyright (c) 2013 HighFidelity, Inc. All rights reserved.
//

#ifndef __hifi__Logstash__
#define __hifi__Logstash__

#ifndef _WIN32
  #include <netinet/in.h>
#endif

const int LOGSTASH_UDP_PORT = 9500;
const char LOGSTASH_HOSTNAME[] = "graphite.highfidelity.io";

const char STAT_TYPE_TIMER = 't';
const char STAT_TYPE_COUNTER = 'c';
const char STAT_TYPE_GAUGE = 'g';

class Logstash {
public:
    static sockaddr* socket();
    static bool shouldSendStats();
    static void stashValue(char statType, const char* key, float value);
private:
    static sockaddr_in logstashSocket;
};

#endif /* defined(__hifi__Logstash__) */
