//
//  AvatarTouch.h
//  interface
//
//  Created by Jeffrey Ventrella
//  Copyright (c) 2012 High Fidelity, Inc. All rights reserved.
//

#ifndef __interface__AvatarTouch__
#define __interface__AvatarTouch__

#include <glm/glm.hpp>

class AvatarTouch {
public:
    AvatarTouch();
    
    void setMyHandPosition  ( glm::vec3 position );
    void setYourHandPosition( glm::vec3 position );
    void setMyHandState     ( int state );
    void setYourHandState   ( int state );
    void simulate           (float deltaTime);
    void render();
    
private:

    static const int NUM_POINTS = 100;
    
    glm::vec3 _point [NUM_POINTS];
    glm::vec3 _myHandPosition;
    glm::vec3 _yourHandPosition;
    int       _myHandState;
    int       _yourHandState;
};

#endif