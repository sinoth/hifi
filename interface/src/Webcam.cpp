//
//  Webcam.cpp
//  interface
//
//  Created by Andrzej Kapolka on 6/17/13.
//  Copyright (c) 2013 High Fidelity, Inc. All rights reserved.

#include <QTimer>
#include <QtDebug>

#include <vpx_encoder.h>
#include <vp8cx.h>

#ifdef __APPLE__
#include <UVCCameraControl.hpp>
#endif

#include <SharedUtil.h>

#include "Application.h"
#include "Webcam.h"

using namespace cv;
using namespace std;

#ifdef HAVE_OPENNI
using namespace xn;
#endif

// register types with Qt metatype system
int jointVectorMetaType = qRegisterMetaType<JointVector>("JointVector");
int matMetaType = qRegisterMetaType<Mat>("cv::Mat");
int rotatedRectMetaType = qRegisterMetaType<RotatedRect>("cv::RotatedRect");

Webcam::Webcam() : _enabled(false), _active(false), _colorTextureID(0), _depthTextureID(0) {
    // the grabber simply runs as fast as possible
    _grabber = new FrameGrabber();
    _grabber->moveToThread(&_grabberThread);
}

void Webcam::setEnabled(bool enabled) {
    if (_enabled == enabled) {
        return;
    }
    if ((_enabled = enabled)) {
        _grabberThread.start();
        _startTimestamp = 0;
        _frameCount = 0;
        
        // let the grabber know we're ready for the first frame
        QMetaObject::invokeMethod(_grabber, "reset");
        QMetaObject::invokeMethod(_grabber, "grabFrame");
    
    } else {
        QMetaObject::invokeMethod(_grabber, "shutdown");
        _active = false;
    }
}

const float UNINITIALIZED_FACE_DEPTH = 0.0f;

void Webcam::reset() {
    _initialFaceRect = RotatedRect();
    _initialFaceDepth = UNINITIALIZED_FACE_DEPTH;
    
    if (_enabled) {
        // send a message to the grabber
        QMetaObject::invokeMethod(_grabber, "reset");
    }
}

void Webcam::renderPreview(int screenWidth, int screenHeight) {
    if (_enabled && _colorTextureID != 0) {
        glBindTexture(GL_TEXTURE_2D, _colorTextureID);
        glEnable(GL_TEXTURE_2D);
        glColor3f(1.0f, 1.0f, 1.0f);
        glBegin(GL_QUADS);
            const int PREVIEW_HEIGHT = 200;
            int previewWidth = _textureSize.width * PREVIEW_HEIGHT / _textureSize.height;
            int top = screenHeight - 600;
            int left = screenWidth - previewWidth - 10;
            
            glTexCoord2f(0, 0);
            glVertex2f(left, top);
            glTexCoord2f(1, 0);
            glVertex2f(left + previewWidth, top);
            glTexCoord2f(1, 1);
            glVertex2f(left + previewWidth, top + PREVIEW_HEIGHT);
            glTexCoord2f(0, 1);
            glVertex2f(left, top + PREVIEW_HEIGHT);
        glEnd();
        
        if (_depthTextureID != 0) {
            glBindTexture(GL_TEXTURE_2D, _depthTextureID);
            glBegin(GL_QUADS);
                glTexCoord2f(0, 0);
                glVertex2f(left, top - PREVIEW_HEIGHT);
                glTexCoord2f(1, 0);
                glVertex2f(left + previewWidth, top - PREVIEW_HEIGHT);
                glTexCoord2f(1, 1);
                glVertex2f(left + previewWidth, top);
                glTexCoord2f(0, 1);
                glVertex2f(left, top);
            glEnd();
            
            glBindTexture(GL_TEXTURE_2D, 0);
            glDisable(GL_TEXTURE_2D);
            
            if (!_joints.isEmpty()) {
                glColor3f(1.0f, 0.0f, 0.0f);
                glPointSize(4.0f);
                glBegin(GL_POINTS);
                    float projectedScale = PREVIEW_HEIGHT / _textureSize.height;
                    foreach (const Joint& joint, _joints) {
                        if (joint.isValid) {
                            glVertex2f(left + joint.projected.x * projectedScale,
                                top - PREVIEW_HEIGHT + joint.projected.y * projectedScale);
                        }
                    }
                glEnd();
                glPointSize(1.0f);
            }
        } else {
            glBindTexture(GL_TEXTURE_2D, 0);
            glDisable(GL_TEXTURE_2D);
        }
        
        glColor3f(1.0f, 1.0f, 1.0f);
        glBegin(GL_LINE_LOOP);
            Point2f facePoints[4];
            _faceRect.points(facePoints);
            float xScale = previewWidth / _textureSize.width;
            float yScale = PREVIEW_HEIGHT / _textureSize.height;
            glVertex2f(left + facePoints[0].x * xScale, top + facePoints[0].y * yScale);
            glVertex2f(left + facePoints[1].x * xScale, top + facePoints[1].y * yScale);
            glVertex2f(left + facePoints[2].x * xScale, top + facePoints[2].y * yScale);
            glVertex2f(left + facePoints[3].x * xScale, top + facePoints[3].y * yScale);
        glEnd();
        
        const int MAX_FPS_CHARACTERS = 30;
        char fps[MAX_FPS_CHARACTERS];
        sprintf(fps, "FPS: %d", (int)(floorf(_frameCount * 1000000.0f / (usecTimestampNow() - _startTimestamp)+0.5f)));
        drawtext(left, top + PREVIEW_HEIGHT + 20, 0.10, 0, 1, 0, fps);
    }
}

Webcam::~Webcam() {
    // stop the grabber thread
    _grabberThread.quit();
    _grabberThread.wait();
    
    delete _grabber;
}

const float METERS_PER_MM = 1.0f / 1000.0f;

void Webcam::setFrame(const Mat& color, int format, const Mat& depth, float meanFaceDepth,
        const RotatedRect& faceRect, const JointVector& joints) {
    IplImage colorImage = color;
    glPixelStorei(GL_UNPACK_ROW_LENGTH, colorImage.widthStep / 3);
    if (_colorTextureID == 0) {
        glGenTextures(1, &_colorTextureID);
        glBindTexture(GL_TEXTURE_2D, _colorTextureID);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, _textureSize.width = colorImage.width, _textureSize.height = colorImage.height,
            0, format, GL_UNSIGNED_BYTE, colorImage.imageData);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        qDebug("Capturing video at %gx%g.\n", _textureSize.width, _textureSize.height);
    
    } else {
        glBindTexture(GL_TEXTURE_2D, _colorTextureID);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, _textureSize.width, _textureSize.height, format,
            GL_UNSIGNED_BYTE, colorImage.imageData);
    }
    
    if (!depth.empty()) {
        IplImage depthImage = depth;
        glPixelStorei(GL_UNPACK_ROW_LENGTH, depthImage.widthStep);
        if (_depthTextureID == 0) {
            glGenTextures(1, &_depthTextureID);
            glBindTexture(GL_TEXTURE_2D, _depthTextureID);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, depthImage.width, depthImage.height, 0,
                GL_LUMINANCE, GL_UNSIGNED_BYTE, depthImage.imageData);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            
        } else {
            glBindTexture(GL_TEXTURE_2D, _depthTextureID);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, _textureSize.width, _textureSize.height, GL_LUMINANCE,
                GL_UNSIGNED_BYTE, depthImage.imageData);        
        }
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    
    // store our face rect and joints, update our frame count for fps computation
    _faceRect = faceRect;
    _joints = joints;
    _frameCount++;
    
    const int MAX_FPS = 60;
    const int MIN_FRAME_DELAY = 1000000 / MAX_FPS;
    uint64_t now = usecTimestampNow();
    int remaining = MIN_FRAME_DELAY;
    if (_startTimestamp == 0) {
        _startTimestamp = now;
    } else {
        remaining -= (now - _lastFrameTimestamp);
    }
    _lastFrameTimestamp = now;
    
    // see if we have joint data
    if (!_joints.isEmpty()) {
        _estimatedJoints.resize(NUM_AVATAR_JOINTS);
        glm::vec3 origin;
        if (_joints[AVATAR_JOINT_LEFT_HIP].isValid && _joints[AVATAR_JOINT_RIGHT_HIP].isValid) {
            origin = glm::mix(_joints[AVATAR_JOINT_LEFT_HIP].position, _joints[AVATAR_JOINT_RIGHT_HIP].position, 0.5f);
        
        } else if (_joints[AVATAR_JOINT_TORSO].isValid) {
            const glm::vec3 TORSO_TO_PELVIS = glm::vec3(0.0f, -0.09f, -0.01f);
            origin = _joints[AVATAR_JOINT_TORSO].position + TORSO_TO_PELVIS;
        }
        for (int i = 0; i < NUM_AVATAR_JOINTS; i++) {
            if (!_joints[i].isValid) {
                continue;
            }
            const float JOINT_SMOOTHING = 0.5f;
            _estimatedJoints[i].isValid = true;
            _estimatedJoints[i].position = glm::mix(_joints[i].position - origin,
                _estimatedJoints[i].position, JOINT_SMOOTHING);
            _estimatedJoints[i].rotation = safeMix(_joints[i].rotation,
                _estimatedJoints[i].rotation, JOINT_SMOOTHING);
        }
        _estimatedRotation = safeEulerAngles(_estimatedJoints[AVATAR_JOINT_HEAD_BASE].rotation);
        _estimatedPosition = _estimatedJoints[AVATAR_JOINT_HEAD_BASE].position;
        
    } else {
        // roll is just the angle of the face rect
        const float ROTATION_SMOOTHING = 0.95f;
        _estimatedRotation.z = glm::mix(_faceRect.angle, _estimatedRotation.z, ROTATION_SMOOTHING);
        
        // determine position based on translation and scaling of the face rect/mean face depth
        if (_initialFaceRect.size.area() == 0) {
            _initialFaceRect = _faceRect;
            _estimatedPosition = glm::vec3();
            _initialFaceDepth = meanFaceDepth;
        
        } else {
            float proportion, z;
            if (meanFaceDepth == UNINITIALIZED_FACE_DEPTH) {
                proportion = sqrtf(_initialFaceRect.size.area() / (float)_faceRect.size.area());
                const float INITIAL_DISTANCE_TO_CAMERA = 0.333f;
                z = INITIAL_DISTANCE_TO_CAMERA * proportion - INITIAL_DISTANCE_TO_CAMERA;           
                   
            } else {
                z = (meanFaceDepth - _initialFaceDepth) * METERS_PER_MM;    
                proportion = meanFaceDepth / _initialFaceDepth;
            }
            const float POSITION_SCALE = 0.5f;
            _estimatedPosition = glm::vec3(
                (_faceRect.center.x - _initialFaceRect.center.x) * proportion * POSITION_SCALE / _textureSize.width,
                (_faceRect.center.y - _initialFaceRect.center.y) * proportion * POSITION_SCALE / _textureSize.width,
                z);
        }
    }
    
    // note that we have data
    _active = true;
    
    // let the grabber know we're ready for the next frame
    QTimer::singleShot(qMax((int)remaining / 1000, 0), _grabber, SLOT(grabFrame()));
}

FrameGrabber::FrameGrabber() : _initialized(false), _capture(0), _searchWindow(0, 0, 0, 0),
    _smoothedMeanFaceDepth(UNINITIALIZED_FACE_DEPTH), _colorCodec(), _depthCodec(), _frameCount(0) {
}

FrameGrabber::~FrameGrabber() {
    if (_initialized) {
        shutdown();
    }
}

#ifdef HAVE_OPENNI
static AvatarJointID xnToAvatarJoint(XnSkeletonJoint joint) {
    switch (joint) {
        case XN_SKEL_HEAD: return AVATAR_JOINT_HEAD_TOP;
        case XN_SKEL_NECK: return AVATAR_JOINT_HEAD_BASE;
        case XN_SKEL_TORSO: return AVATAR_JOINT_CHEST;
        
        case XN_SKEL_LEFT_SHOULDER: return AVATAR_JOINT_RIGHT_ELBOW;
        case XN_SKEL_LEFT_ELBOW: return AVATAR_JOINT_RIGHT_WRIST;
        
        case XN_SKEL_RIGHT_SHOULDER: return AVATAR_JOINT_LEFT_ELBOW;
        case XN_SKEL_RIGHT_ELBOW: return AVATAR_JOINT_LEFT_WRIST;
        
        case XN_SKEL_LEFT_HIP: return AVATAR_JOINT_RIGHT_KNEE;
        case XN_SKEL_LEFT_KNEE: return AVATAR_JOINT_RIGHT_HEEL;
        case XN_SKEL_LEFT_FOOT: return AVATAR_JOINT_RIGHT_TOES;
        
        case XN_SKEL_RIGHT_HIP: return AVATAR_JOINT_LEFT_KNEE;
        case XN_SKEL_RIGHT_KNEE: return AVATAR_JOINT_LEFT_HEEL;
        case XN_SKEL_RIGHT_FOOT: return AVATAR_JOINT_LEFT_TOES;
        
        default: return AVATAR_JOINT_NULL;
    }
}

static int getParentJoint(XnSkeletonJoint joint) {
    switch (joint) {
        case XN_SKEL_HEAD: return XN_SKEL_NECK;
        case XN_SKEL_TORSO: return -1;
        
        case XN_SKEL_LEFT_ELBOW: return XN_SKEL_LEFT_SHOULDER;
        case XN_SKEL_LEFT_HAND: return XN_SKEL_LEFT_ELBOW;
        
        case XN_SKEL_RIGHT_ELBOW: return XN_SKEL_RIGHT_SHOULDER;
        case XN_SKEL_RIGHT_HAND: return XN_SKEL_RIGHT_ELBOW;
        
        case XN_SKEL_LEFT_KNEE: return XN_SKEL_LEFT_HIP;
        case XN_SKEL_LEFT_FOOT: return XN_SKEL_LEFT_KNEE;
        
        case XN_SKEL_RIGHT_KNEE: return XN_SKEL_RIGHT_HIP;
        case XN_SKEL_RIGHT_FOOT: return XN_SKEL_RIGHT_KNEE;
        
        default: return XN_SKEL_TORSO;
    }
}

static glm::vec3 xnToGLM(const XnVector3D& vector, bool flip = false) {
    return glm::vec3(vector.X * (flip ? -1 : 1), vector.Y, vector.Z);
}

static glm::quat xnToGLM(const XnMatrix3X3& matrix) {
    glm::quat rotation = glm::quat_cast(glm::mat3(
        matrix.elements[0], matrix.elements[1], matrix.elements[2],
        matrix.elements[3], matrix.elements[4], matrix.elements[5],
        matrix.elements[6], matrix.elements[7], matrix.elements[8]));
    return glm::quat(rotation.w, -rotation.x, rotation.y, rotation.z);
}

static void XN_CALLBACK_TYPE newUser(UserGenerator& generator, XnUserID id, void* cookie) {
    qDebug("Found user %d.\n", id);
    generator.GetSkeletonCap().RequestCalibration(id, false);
}

static void XN_CALLBACK_TYPE lostUser(UserGenerator& generator, XnUserID id, void* cookie) {
    qDebug("Lost user %d.\n", id);
}

static void XN_CALLBACK_TYPE calibrationStarted(SkeletonCapability& capability, XnUserID id, void* cookie) {
    qDebug("Calibration started for user %d.\n", id);
}

static void XN_CALLBACK_TYPE calibrationCompleted(SkeletonCapability& capability,
        XnUserID id, XnCalibrationStatus status, void* cookie) {
    if (status == XN_CALIBRATION_STATUS_OK) {
        qDebug("Calibration completed for user %d.\n", id);
        capability.StartTracking(id);
    
    } else {
        qDebug("Calibration failed to user %d.\n", id);
        capability.RequestCalibration(id, true);
    }
}
#endif

void FrameGrabber::reset() {
    _searchWindow = cv::Rect(0, 0, 0, 0);

#ifdef HAVE_OPENNI
    if (_userGenerator.IsValid() && _userGenerator.GetSkeletonCap().IsTracking(_userID)) {
        _userGenerator.GetSkeletonCap().RequestCalibration(_userID, true);
    }
#endif
}

void FrameGrabber::shutdown() {
    if (_capture != 0) {
        cvReleaseCapture(&_capture);
        _capture = 0;
    }
    if (_colorCodec.name != 0) {
        vpx_codec_destroy(&_colorCodec);
        _colorCodec.name = 0;
    }
    if (_depthCodec.name != 0) {
        vpx_codec_destroy(&_depthCodec);
        _depthCodec.name = 0;
    }
    _initialized = false;
    
    thread()->quit();
}

static Point clip(const Point& point, const Rect& bounds) {
    return Point(glm::clamp(point.x, bounds.x, bounds.x + bounds.width - 1),
        glm::clamp(point.y, bounds.y, bounds.y + bounds.height - 1));
}

void FrameGrabber::grabFrame() {
    if (!(_initialized || init())) {
        return;
    }
    int format = GL_BGR;
    Mat color, depth;
    JointVector joints;
    
#ifdef HAVE_OPENNI
    if (_depthGenerator.IsValid()) {
        _xnContext.WaitAnyUpdateAll();
        color = Mat(_imageMetaData.YRes(), _imageMetaData.XRes(), CV_8UC3, (void*)_imageGenerator.GetImageMap());
        format = GL_RGB;
        
        depth = Mat(_depthMetaData.YRes(), _depthMetaData.XRes(), CV_16UC1, (void*)_depthGenerator.GetDepthMap());
        
        _userID = 0;
        XnUInt16 userCount = 1; 
        _userGenerator.GetUsers(&_userID, userCount);
        if (userCount > 0 && _userGenerator.GetSkeletonCap().IsTracking(_userID)) {
            joints.resize(NUM_AVATAR_JOINTS);
            const int MAX_ACTIVE_JOINTS = 16;
            XnSkeletonJoint activeJoints[MAX_ACTIVE_JOINTS];
            XnUInt16 activeJointCount = MAX_ACTIVE_JOINTS;
            _userGenerator.GetSkeletonCap().EnumerateActiveJoints(activeJoints, activeJointCount);
            XnSkeletonJointTransformation transform;
            for (int i = 0; i < activeJointCount; i++) {
                AvatarJointID avatarJoint = xnToAvatarJoint(activeJoints[i]);
                if (avatarJoint == AVATAR_JOINT_NULL) {
                    continue;
                }
                _userGenerator.GetSkeletonCap().GetSkeletonJoint(_userID, activeJoints[i], transform);
                XnVector3D projected;
                _depthGenerator.ConvertRealWorldToProjective(1, &transform.position.position, &projected);
                glm::quat rotation = xnToGLM(transform.orientation.orientation);
                int parentJoint = getParentJoint(activeJoints[i]);
                if (parentJoint != -1) {
                    XnSkeletonJointOrientation parentOrientation;
                    _userGenerator.GetSkeletonCap().GetSkeletonJointOrientation(
                        _userID, (XnSkeletonJoint)parentJoint, parentOrientation);
                    rotation = glm::inverse(xnToGLM(parentOrientation.orientation)) * rotation;
                }
                joints[avatarJoint] = Joint(xnToGLM(transform.position.position, true) * METERS_PER_MM,
                    rotation, xnToGLM(projected));
            }
        }
    }
#endif

    if (color.empty()) {
        IplImage* image = cvQueryFrame(_capture);
        if (image == 0) {
            // try again later
            QMetaObject::invokeMethod(this, "grabFrame", Qt::QueuedConnection);
            return;
        }
        // make sure it's in the format we expect
        if (image->nChannels != 3 || image->depth != IPL_DEPTH_8U || image->dataOrder != IPL_DATA_ORDER_PIXEL ||
                image->origin != 0) {
            qDebug("Invalid webcam image format.\n");
            return;
        }
        color = image;
    }
    
    // if we don't have a search window (yet), try using the face cascade
    int channels = 0;
    float ranges[] = { 0, 180 };
    const float* range = ranges;
    if (_searchWindow.area() == 0) {
        vector<Rect> faces;
        _faceCascade.detectMultiScale(color, faces, 1.1, 6);
        if (!faces.empty()) {
            _searchWindow = faces.front();
            updateHSVFrame(color, format);
        
            Mat faceHsv(_hsvFrame, _searchWindow);
            Mat faceMask(_mask, _searchWindow);
            int sizes = 30;
            calcHist(&faceHsv, 1, &channels, faceMask, _histogram, 1, &sizes, &range);
            double min, max;
            minMaxLoc(_histogram, &min, &max);
            _histogram.convertTo(_histogram, -1, (max == 0.0) ? 0.0 : 255.0 / max);
        }
    }
    RotatedRect faceRect;
    if (_searchWindow.area() > 0) {
        updateHSVFrame(color, format);
        
        calcBackProject(&_hsvFrame, 1, &channels, _histogram, _backProject, &range);
        bitwise_and(_backProject, _mask, _backProject);
        
        faceRect = CamShift(_backProject, _searchWindow, TermCriteria(CV_TERMCRIT_EPS | CV_TERMCRIT_ITER, 10, 1));
        Rect faceBounds = faceRect.boundingRect();
        Rect imageBounds(0, 0, color.cols, color.rows);
        _searchWindow = Rect(clip(faceBounds.tl(), imageBounds), clip(faceBounds.br(), imageBounds));
    }

    const int ENCODED_FACE_WIDTH = 128;
    const int ENCODED_FACE_HEIGHT = 128;
    if (_colorCodec.name == 0) {
        // initialize encoder context(s)
        vpx_codec_enc_cfg_t codecConfig;
        vpx_codec_enc_config_default(vpx_codec_vp8_cx(), &codecConfig, 0);
        codecConfig.rc_target_bitrate = ENCODED_FACE_WIDTH * ENCODED_FACE_HEIGHT *
            codecConfig.rc_target_bitrate / codecConfig.g_w / codecConfig.g_h;
        codecConfig.g_w = ENCODED_FACE_WIDTH;
        codecConfig.g_h = ENCODED_FACE_HEIGHT;
        vpx_codec_enc_init(&_colorCodec, vpx_codec_vp8_cx(), &codecConfig, 0);
        
        if (!depth.empty()) {
            int DEPTH_BITRATE_MULTIPLIER = 2;
            codecConfig.rc_target_bitrate *= 2;
            vpx_codec_enc_init(&_depthCodec, vpx_codec_vp8_cx(), &codecConfig, 0);
        }
    }
    
    // correct for 180 degree rotations
    if (faceRect.angle < -90.0f) {
        faceRect.angle += 180.0f;
        
    } else if (faceRect.angle > 90.0f) {
        faceRect.angle -= 180.0f;
    }
    
    // compute the smoothed face rect
    if (_smoothedFaceRect.size.area() == 0) {
        _smoothedFaceRect = faceRect;
        
    } else {
        const float FACE_RECT_SMOOTHING = 0.9f;
        _smoothedFaceRect.center.x = glm::mix(faceRect.center.x, _smoothedFaceRect.center.x, FACE_RECT_SMOOTHING);
        _smoothedFaceRect.center.y = glm::mix(faceRect.center.y, _smoothedFaceRect.center.y, FACE_RECT_SMOOTHING);
        _smoothedFaceRect.size.width = glm::mix(faceRect.size.width, _smoothedFaceRect.size.width, FACE_RECT_SMOOTHING);
        _smoothedFaceRect.size.height = glm::mix(faceRect.size.height, _smoothedFaceRect.size.height, FACE_RECT_SMOOTHING); 
        _smoothedFaceRect.angle = glm::mix(faceRect.angle, _smoothedFaceRect.angle, FACE_RECT_SMOOTHING);
    }
    
    // resize/rotate face into encoding rectangle
    _faceColor.create(ENCODED_FACE_WIDTH, ENCODED_FACE_HEIGHT, CV_8UC3);
    Point2f sourcePoints[4];
    _smoothedFaceRect.points(sourcePoints);
    Point2f destPoints[] = { Point2f(0, ENCODED_FACE_HEIGHT), Point2f(0, 0), Point2f(ENCODED_FACE_WIDTH, 0) };
    Mat transform = getAffineTransform(sourcePoints, destPoints);
    warpAffine(color, _faceColor, transform, _faceColor.size());
    
    // convert from RGB to YV12
    const int ENCODED_BITS_PER_Y = 8;
    const int ENCODED_BITS_PER_VU = 2;
    const int ENCODED_BITS_PER_PIXEL = ENCODED_BITS_PER_Y + 2 * ENCODED_BITS_PER_VU;
    const int BITS_PER_BYTE = 8;
    _encodedFace.resize(ENCODED_FACE_WIDTH * ENCODED_FACE_HEIGHT * ENCODED_BITS_PER_PIXEL / BITS_PER_BYTE);
    vpx_image_t vpxImage;
    vpx_img_wrap(&vpxImage, VPX_IMG_FMT_YV12, ENCODED_FACE_WIDTH, ENCODED_FACE_HEIGHT, 1, (unsigned char*)_encodedFace.data());
    uchar* yline = vpxImage.planes[0];
    uchar* vline = vpxImage.planes[1];
    uchar* uline = vpxImage.planes[2];
    const int Y_RED_WEIGHT = (int)(0.299 * 256);
    const int Y_GREEN_WEIGHT = (int)(0.587 * 256);
    const int Y_BLUE_WEIGHT = (int)(0.114 * 256);
    const int V_RED_WEIGHT = (int)(0.713 * 256);
    const int U_BLUE_WEIGHT = (int)(0.564 * 256);
    int redIndex = 0;
    int greenIndex = 1;
    int blueIndex = 2;
    if (format == GL_BGR) {
        redIndex = 2;
        blueIndex = 0;
    }
    for (int i = 0; i < ENCODED_FACE_HEIGHT; i += 2) {
        uchar* ydest = yline;
        uchar* vdest = vline;
        uchar* udest = uline;
        for (int j = 0; j < ENCODED_FACE_WIDTH; j += 2) {
            uchar* tl = _faceColor.ptr(i, j);
            uchar* tr = _faceColor.ptr(i, j + 1);
            uchar* bl = _faceColor.ptr(i + 1, j);
            uchar* br = _faceColor.ptr(i + 1, j + 1);
            
            ydest[0] = (tl[redIndex] * Y_RED_WEIGHT + tl[1] * Y_GREEN_WEIGHT + tl[blueIndex] * Y_BLUE_WEIGHT) >> 8;
            ydest[1] = (tr[redIndex] * Y_RED_WEIGHT + tr[1] * Y_GREEN_WEIGHT + tr[blueIndex] * Y_BLUE_WEIGHT) >> 8;
            ydest[vpxImage.stride[0]] = (bl[redIndex] * Y_RED_WEIGHT + bl[greenIndex] *
                Y_GREEN_WEIGHT + bl[blueIndex] * Y_BLUE_WEIGHT) >> 8;
            ydest[vpxImage.stride[0] + 1] = (br[redIndex] * Y_RED_WEIGHT + br[greenIndex] *
                Y_GREEN_WEIGHT + br[blueIndex] * Y_BLUE_WEIGHT) >> 8;
            ydest += 2;
            
            int totalRed = tl[redIndex] + tr[redIndex] + bl[redIndex] + br[redIndex];
            int totalGreen = tl[greenIndex] + tr[greenIndex] + bl[greenIndex] + br[greenIndex];
            int totalBlue = tl[blueIndex] + tr[blueIndex] + bl[blueIndex] + br[blueIndex];
            int totalY = (totalRed * Y_RED_WEIGHT + totalGreen * Y_GREEN_WEIGHT + totalBlue * Y_BLUE_WEIGHT) >> 8;
            
            *vdest++ = (((totalRed - totalY) * V_RED_WEIGHT) >> 10) + 128;
            *udest++ = (((totalBlue - totalY) * U_BLUE_WEIGHT) >> 10) + 128;
        }
        yline += vpxImage.stride[0] * 2;
        vline += vpxImage.stride[1];
        uline += vpxImage.stride[2];
    }
    
    // encode the frame
    vpx_codec_encode(&_colorCodec, &vpxImage, ++_frameCount, 1, 0, VPX_DL_REALTIME);

    // start the payload off with the aspect ratio
    QByteArray payload(sizeof(float), 0);
    *(float*)payload.data() = _smoothedFaceRect.size.width / _smoothedFaceRect.size.height;

    // extract the encoded frame
    vpx_codec_iter_t iterator = 0;
    const vpx_codec_cx_pkt_t* packet;
    while ((packet = vpx_codec_get_cx_data(&_colorCodec, &iterator)) != 0) {
        if (packet->kind == VPX_CODEC_CX_FRAME_PKT) {
            // prepend the length, which will indicate whether there's a depth frame too
            payload.append((const char*)&packet->data.frame.sz, sizeof(packet->data.frame.sz));
            payload.append((const char*)packet->data.frame.buf, packet->data.frame.sz);
        }
    }
    
    if (!depth.empty()) {
        // warp the face depth without interpolation (because it will contain invalid zero values)
        _faceDepth.create(ENCODED_FACE_WIDTH, ENCODED_FACE_HEIGHT, CV_16UC1);
        warpAffine(depth, _faceDepth, transform, _faceDepth.size(), INTER_NEAREST);
        
        // find the mean of the valid values
        qint64 depthTotal = 0;
        qint64 depthSamples = 0;
        ushort* src = _faceDepth.ptr<ushort>();
        const ushort ELEVEN_BIT_MINIMUM = 0;
        const ushort ELEVEN_BIT_MAXIMUM = 2047;
        for (int i = 0; i < ENCODED_FACE_HEIGHT; i++) {
            for (int j = 0; j < ENCODED_FACE_WIDTH; j++) {
                ushort depth = *src++;
                if (depth != ELEVEN_BIT_MINIMUM && depth != ELEVEN_BIT_MAXIMUM) {
                    depthTotal += depth;
                    depthSamples++;
                }
            }
        }
        float mean = (depthSamples == 0) ? UNINITIALIZED_FACE_DEPTH : depthTotal / (float)depthSamples;
        
        // smooth the mean over time
        const float DEPTH_OFFSET_SMOOTHING = 0.95f;
        _smoothedMeanFaceDepth = (_smoothedMeanFaceDepth == UNINITIALIZED_FACE_DEPTH) ? mean :
            glm::mix(mean, _smoothedMeanFaceDepth, DEPTH_OFFSET_SMOOTHING);

        // convert from 11 to 8 bits for preview/local display
        const uchar EIGHT_BIT_MIDPOINT = 128;
        double depthOffset = EIGHT_BIT_MIDPOINT - _smoothedMeanFaceDepth;
        depth.convertTo(_grayDepthFrame, CV_8UC1, 1.0, depthOffset);

        // likewise for the encoded representation
        uchar* yline = vpxImage.planes[0];
        uchar* vline = vpxImage.planes[1];
        uchar* uline = vpxImage.planes[2];
        const uchar EIGHT_BIT_MAXIMUM = 255;
        for (int i = 0; i < ENCODED_FACE_HEIGHT; i += 2) {
            uchar* ydest = yline;
            uchar* vdest = vline;
            uchar* udest = uline;
            for (int j = 0; j < ENCODED_FACE_WIDTH; j += 2) {
                ushort tl = *_faceDepth.ptr<ushort>(i, j);
                ushort tr = *_faceDepth.ptr<ushort>(i, j + 1);
                ushort bl = *_faceDepth.ptr<ushort>(i + 1, j);
                ushort br = *_faceDepth.ptr<ushort>(i + 1, j + 1);
            
                uchar mask = EIGHT_BIT_MAXIMUM;
                
                ydest[0] = (tl == ELEVEN_BIT_MINIMUM) ? (mask = EIGHT_BIT_MIDPOINT) : saturate_cast<uchar>(tl + depthOffset);
                ydest[1] = (tr == ELEVEN_BIT_MINIMUM) ? (mask = EIGHT_BIT_MIDPOINT) : saturate_cast<uchar>(tr + depthOffset);
                ydest[vpxImage.stride[0]] = (bl == ELEVEN_BIT_MINIMUM) ?
                    (mask = EIGHT_BIT_MIDPOINT) : saturate_cast<uchar>(bl + depthOffset);
                ydest[vpxImage.stride[0] + 1] = (br == ELEVEN_BIT_MINIMUM) ?
                    (mask = EIGHT_BIT_MIDPOINT) : saturate_cast<uchar>(br + depthOffset);
                ydest += 2;
            
                *vdest++ = mask;
                *udest++ = EIGHT_BIT_MIDPOINT;
            }
            yline += vpxImage.stride[0] * 2;
            vline += vpxImage.stride[1];
            uline += vpxImage.stride[2];
        }
        
        // encode the frame
        vpx_codec_encode(&_depthCodec, &vpxImage, _frameCount, 1, 0, VPX_DL_REALTIME);

        // extract the encoded frame
        vpx_codec_iter_t iterator = 0;
        const vpx_codec_cx_pkt_t* packet;
        while ((packet = vpx_codec_get_cx_data(&_depthCodec, &iterator)) != 0) {
            if (packet->kind == VPX_CODEC_CX_FRAME_PKT) {
                payload.append((const char*)packet->data.frame.buf, packet->data.frame.sz);
            }
        }
    }
    
    QMetaObject::invokeMethod(Application::getInstance(), "sendAvatarFaceVideoMessage",
        Q_ARG(int, _frameCount), Q_ARG(QByteArray, payload));

    QMetaObject::invokeMethod(Application::getInstance()->getWebcam(), "setFrame",
        Q_ARG(cv::Mat, color), Q_ARG(int, format), Q_ARG(cv::Mat, _grayDepthFrame), Q_ARG(float, _smoothedMeanFaceDepth),
        Q_ARG(cv::RotatedRect, _smoothedFaceRect), Q_ARG(JointVector, joints));
}

bool FrameGrabber::init() {
    _initialized = true;

    // load our face cascade
    switchToResourcesParentIfRequired();
    if (_faceCascade.empty() && !_faceCascade.load("resources/haarcascades/haarcascade_frontalface_alt.xml")) {
        qDebug("Failed to load Haar cascade for face tracking.\n");
        return false;
    }

    // first try for a Kinect
#ifdef HAVE_OPENNI
    _xnContext.Init();
    if (_depthGenerator.Create(_xnContext) == XN_STATUS_OK && _imageGenerator.Create(_xnContext) == XN_STATUS_OK &&
            _userGenerator.Create(_xnContext) == XN_STATUS_OK &&
                _userGenerator.IsCapabilitySupported(XN_CAPABILITY_SKELETON)) {
        _depthGenerator.GetMetaData(_depthMetaData);
        _imageGenerator.SetPixelFormat(XN_PIXEL_FORMAT_RGB24);
        _imageGenerator.GetMetaData(_imageMetaData);
        
        XnCallbackHandle userCallbacks, calibrationStartCallback, calibrationCompleteCallback;
        _userGenerator.RegisterUserCallbacks(newUser, lostUser, 0, userCallbacks);
        _userGenerator.GetSkeletonCap().RegisterToCalibrationStart(calibrationStarted, 0, calibrationStartCallback);
        _userGenerator.GetSkeletonCap().RegisterToCalibrationComplete(calibrationCompleted, 0, calibrationCompleteCallback);
        
        _userGenerator.GetSkeletonCap().SetSkeletonProfile(XN_SKEL_PROFILE_UPPER);
        
        // make the depth viewpoint match that of the video image
        if (_depthGenerator.IsCapabilitySupported(XN_CAPABILITY_ALTERNATIVE_VIEW_POINT)) {
            _depthGenerator.GetAlternativeViewPointCap().SetViewPoint(_imageGenerator);
        }
        
        _xnContext.StartGeneratingAll();
        return true;
    }
#endif

    // next, an ordinary webcam
    if ((_capture = cvCaptureFromCAM(-1)) == 0) {
        qDebug("Failed to open webcam.\n");
        return false;
    }
    const int IDEAL_FRAME_WIDTH = 320;
    const int IDEAL_FRAME_HEIGHT = 240;
    cvSetCaptureProperty(_capture, CV_CAP_PROP_FRAME_WIDTH, IDEAL_FRAME_WIDTH);
    cvSetCaptureProperty(_capture, CV_CAP_PROP_FRAME_HEIGHT, IDEAL_FRAME_HEIGHT);
    
#ifdef __APPLE__
    configureCamera(0x5ac, 0x8510, false, 0.975, 0.5, 1.0, 0.5, true, 0.5);
#else
    cvSetCaptureProperty(_capture, CV_CAP_PROP_EXPOSURE, 0.5);
    cvSetCaptureProperty(_capture, CV_CAP_PROP_CONTRAST, 0.5);
    cvSetCaptureProperty(_capture, CV_CAP_PROP_SATURATION, 0.5);
    cvSetCaptureProperty(_capture, CV_CAP_PROP_BRIGHTNESS, 0.5);
    cvSetCaptureProperty(_capture, CV_CAP_PROP_HUE, 0.5);
    cvSetCaptureProperty(_capture, CV_CAP_PROP_GAIN, 0.5);
#endif

    return true;
}

void FrameGrabber::updateHSVFrame(const Mat& frame, int format) {
    cvtColor(frame, _hsvFrame, format == GL_RGB ? CV_RGB2HSV : CV_BGR2HSV);
    inRange(_hsvFrame, Scalar(0, 55, 65), Scalar(180, 256, 256), _mask);
}

Joint::Joint(const glm::vec3& position, const glm::quat& rotation, const glm::vec3& projected) :
    isValid(true), position(position), rotation(rotation), projected(projected) {
}

Joint::Joint() : isValid(false) {
}
