#include <Arduino.h>
#include <Wire.h>
#include <ZumoShield.h>

//Quick shift variable for edge color. Black edge = > , white edge = <
#define COLOR_EDGE >

// Define pins to variable names
const int ON_BOARD_LED = 13;

const int NUM_SENSORS = 6;
unsigned int sensor_values[NUM_SENSORS];

// this might need to be tuned for different lighting conditions, surfaces, etc.
const int QTR_THRESHOLD = 1500; // microseconds

// Speeds: define different speed levels
// 0-400 : 400 Full speed

//Value 1 for normal speed, value 2 for testing speed.
const int SPEED_CONTROL = 1;

const int FULL_SPEED =         400/SPEED_CONTROL;
const int FULL_REVERSE_SPEED = 350/SPEED_CONTROL;
const int REVERSE_SPEED =      250/SPEED_CONTROL;
const int TURN_SPEED =         200/SPEED_CONTROL;
const int FORWARD_SPEED =      100/SPEED_CONTROL;
const int SEARCH_SPEED =       100/SPEED_CONTROL;
const int SUSTAINED_SPEED =    50/SPEED_CONTROL; // switches to SUSTAINED_SPEED from FULL_SPEED after FULL_SPEED_DURATION_LIMIT ms

// Duration : Timing constants
const int REVERSE_DURATION =   300; // ms
const int TURN_DURATION =      350; // ms

// Timing for accelerometer
unsigned long loop_start_time;
unsigned long last_turn_time;
unsigned long contact_made_time;
const int MIN_DELAY_AFTER_TURN     =    400;  // ms = min delay before detecting contact event
const int MIN_DELAY_BETWEEN_CONTACTS =  1000;  // ms = min delay between detecting new contact event

// Directions
const int RIGHT = 1;
const int LEFT = -1;

// Accelerometer Settings
const int RA_SIZE = 3;  // number of readings to include in running average of accelerometer readings
const int XY_ACCELERATION_THRESHOLD = 2200;  // for detection of contact (~16000 = magnitude of acceleration due to gravity)

enum ForwardSpeed { SearchSpeed, SustainedSpeed, FullSpeed, SlowSpeed };
ForwardSpeed _forwardSpeed;  // current forward speed setting
unsigned long full_speed_start_time;
const int FULL_SPEED_DURATION_LIMIT   = 250;  // ms



// RunningAverage class
// based on RunningAverage library for Arduino
// source:  https://playground.arduino.cc/Main/RunningAverage
template <typename T>
class RunningAverage
{
  public:
    RunningAverage(void);
    RunningAverage(int);
    ~RunningAverage();
    void clear();
    void addValue(T);
    T getAverage() const;
    void fillValue(T, int);
  protected:
    int _size;
    int _cnt;
    int _idx;
    T _sum;
    T * _ar;
    static T zero;
};

// Accelerometer Class -- extends the LSM303 Library to support reading and averaging the x-y acceleration
//   vectors from the onboard LSM303DLHC accelerometer/magnetometer
class Accelerometer : public LSM303
{
  typedef struct acc_data_xy
  {
    unsigned long timestamp;
    int x;
    int y;
    float dir;
  } acc_data_xy;

  public:
    Accelerometer() : ra_x(RA_SIZE), ra_y(RA_SIZE) {};
    ~Accelerometer() {};
    void enable(void);
    void getLogHeader(void);
    void readAcceleration(unsigned long timestamp);
    float len_xy() const;
    float dir_xy() const;
    int x_avg(void) const;
    int y_avg(void) const;
    long ss_xy_avg(void) const;
    float dir_xy_avg(void) const;
  private:
    acc_data_xy last;
    RunningAverage<int> ra_x;
    RunningAverage<int> ra_y;
};

Accelerometer lsm303;
boolean in_contact;  // set when accelerometer detects contact with opposing robot

// –––––––––––––––––––––––––––––––––––––


ZumoBuzzer buzzer;
const char sound_effect[] PROGMEM = "O4 T100 V15 L4 MS g12>c12>e12>G6>E12 ML>G2"; // "charge" melody
 // use V0 to suppress sound effect; v15 for max volume
ZumoMotors motors;
Pushbutton button(ZUMO_BUTTON); // pushbutton on pin 12
ZumoReflectanceSensorArray sensors(QTR_NO_EMITTER_PIN);


unsigned int nextTimeout = 0;

// STATES

const int S_STANDBY = 0;
const int S_FLIGHT = 1;
const int S_SCOUT = 2;

int state = S_STANDBY;


void setup()
{
  Wire.begin();

  digitalWrite(6, HIGH);

  // Initiate LSM303
  lsm303.init();
  lsm303.enable();
  // uncomment if necessary to correct motor directions
  //motors.flipLeftMotor(true);
  //motors.flipRightMotor(true);

  pinMode(ON_BOARD_LED, HIGH);

  Serial.begin(9600);
  lsm303.getLogHeader();

  // reset loop variables
  in_contact = false;  // 1 if contact made; 0 if no contact or contact lost
  contact_made_time = 0;
  last_turn_time = millis();  // prevents false contact detection on initial acceleration
  _forwardSpeed = SlowSpeed;
  full_speed_start_time = 0;
}

void waitForButtonAndCountDown()
{
  button.waitForPress();
  digitalWrite(ON_BOARD_LED, HIGH);
  button.waitForRelease();
  digitalWrite(ON_BOARD_LED, LOW);

  // play audible countdown
  for (int i = 0; i < 3; i++)
  {
    delay(1000);
    buzzer.playNote(NOTE_G(3), 200, 15);
  }
  delay(1000);
  buzzer.playNote(NOTE_G(4), 500, 15);


}

/*
  Takes desired length of timeout and sets the global variable to hold the value.
*/

void startTimer(unsigned long timeout) {
  nextTimeout = millis() + timeout;
}

/*
  Checks if the timeout has expired. Uses startTimer to set a value of timeout.
*/

bool isTimerExpired() {
  bool timerHasExpired = false;

  if (millis() > nextTimeout) {
    timerHasExpired = true;
  } else {
    timerHasExpired = false;
  }
  return timerHasExpired;
}

// Speed functions

void setForwardSpeed(ForwardSpeed speed)
{
  Serial.println("YES");
  Serial.println(speed);
  _forwardSpeed = speed;
  if (speed == FullSpeed) {
    full_speed_start_time = loop_start_time;
  }
}

int getForwardSpeed()
{
  int speed;
  switch (_forwardSpeed)
  {
    case FullSpeed:
      speed = FULL_SPEED;
      break;
    case SlowSpeed:
      speed = FORWARD_SPEED;
      break;
    case SustainedSpeed:
      speed = SUSTAINED_SPEED;
      break;
    default:
      speed = SEARCH_SPEED;
      break;
  }
  return speed;
}



// sound horn and accelerate on contact -- fight or flight
void on_contact_made()
{
  Serial.print("contact made");
  Serial.println();

  in_contact = true;
  Serial.println("IN CONTACT = TRUE");
  contact_made_time = loop_start_time;
  Serial.println(contact_made_time);
  setForwardSpeed(FullSpeed);
  buzzer.playFromProgramSpace(sound_effect);
}

// reset forward speed
void on_contact_lost()
{
  in_contact = false;
  setForwardSpeed(SearchSpeed);
}


// check for contact, but ignore readings immediately after turning or losing contact
bool check_for_contact()
{
  static long threshold_squared = (long) XY_ACCELERATION_THRESHOLD * (long) XY_ACCELERATION_THRESHOLD;
  return (lsm303.ss_xy_avg() >  threshold_squared) && \
    (loop_start_time - last_turn_time > MIN_DELAY_AFTER_TURN) && \
    (loop_start_time - contact_made_time > MIN_DELAY_BETWEEN_CONTACTS);
}


// class Accelerometer -- member function definitions

// enable accelerometer only
// to enable both accelerometer and magnetometer, call enableDefault() instead
void Accelerometer::enable(void)
{
  // Enable Accelerometer
  // 0x27 = 0b00100111
  // Normal power mode, all axes enabled
  writeAccReg(LSM303::CTRL_REG1_A, 0x27);

  if (getDeviceType() == LSM303::device_DLHC)
  writeAccReg(LSM303::CTRL_REG4_A, 0x08); // DLHC: enable high resolution mode
}

void Accelerometer::getLogHeader(void)
{
  Serial.print("millis    x      y     len     dir  | len_avg  dir_avg  |  avg_len");
  Serial.println();
}

void Accelerometer::readAcceleration(unsigned long timestamp)
{
  readAcc();
  if (a.x == last.x && a.y == last.y) return;

  last.timestamp = timestamp;
  last.x = a.x;
  last.y = a.y;

  ra_x.addValue(last.x);
  ra_y.addValue(last.y);
  }

float Accelerometer::len_xy() const
{
  return sqrt(last.x*a.x + last.y*a.y);
}

float Accelerometer::dir_xy() const
{
  return atan2(last.x, last.y) * 180.0 / M_PI;
}

int Accelerometer::x_avg(void) const
{
  return ra_x.getAverage();
}

int Accelerometer::y_avg(void) const
{
  return ra_y.getAverage();
}

long Accelerometer::ss_xy_avg(void) const
{
  long x_avg_long = static_cast<long>(x_avg());
  long y_avg_long = static_cast<long>(y_avg());
  return x_avg_long*x_avg_long + y_avg_long*y_avg_long;
}

float Accelerometer::dir_xy_avg(void) const
{
  return atan2(static_cast<float>(x_avg()), static_cast<float>(y_avg())) * 180.0 / M_PI;
}

// RunningAverage class
// based on RunningAverage library for Arduino
// source:  https://playground.arduino.cc/Main/RunningAverage
// author:  Rob.Tillart@gmail.com
// Released to the public domain

template <typename T>
T RunningAverage<T>::zero = static_cast<T>(0);

template <typename T>
RunningAverage<T>::RunningAverage(int n)
{
  _size = n;
  _ar = (T*) malloc(_size * sizeof(T));
  clear();
}

template <typename T>
RunningAverage<T>::~RunningAverage()
{
  free(_ar);
}

// resets all counters
template <typename T>
void RunningAverage<T>::clear()
{
  _cnt = 0;
  _idx = 0;
  _sum = zero;
  for (int i = 0; i< _size; i++) _ar[i] = zero;  // needed to keep addValue simple
}

// adds a new value to the data-set
template <typename T>
void RunningAverage<T>::addValue(T f)
{
  _sum -= _ar[_idx];
  _ar[_idx] = f;
  _sum += _ar[_idx];
  _idx++;
  if (_idx == _size) _idx = 0;  // faster than %
  if (_cnt < _size) _cnt++;
}

// returns the average of the data-set added so far
template <typename T>
T RunningAverage<T>::getAverage() const
{
  if (_cnt == 0) return zero; // NaN ?  math.h
  return _sum / _cnt;
}

// fill the average with a value
// the param number determines how often value is added (weight)
// number should preferably be between 1 and size
template <typename T>
void RunningAverage<T>::fillValue(T value, int number)
{
  clear();
  for (int i = 0; i < number; i++)
  {
    addValue(value);
  }
}

// execute turn
// direction:  RIGHT or LEFT
// randomize: to improve searching
void turn(char direction, bool randomize) {
  // assume contact lost
  on_contact_lost();

  static unsigned int duration_increment = TURN_DURATION / 4;

  // motors.setSpeeds(0,0);
  // delay(STOP_DURATION);
  motors.setSpeeds(-REVERSE_SPEED, -REVERSE_SPEED);
  delay(REVERSE_DURATION);
  motors.setSpeeds(TURN_SPEED * direction, -TURN_SPEED * direction);
  delay(randomize ? TURN_DURATION + (random(8) - 2) * duration_increment : TURN_DURATION);
  int speed = getForwardSpeed();
  motors.setSpeeds(speed, speed);
  last_turn_time = millis();
}


void loop()
{

  loop_start_time = millis();
  lsm303.readAcceleration(loop_start_time);
  sensors.read(sensor_values);
  int valFromIRSensorLeft = analogRead(A0);
  int valFromIRSensorRight = analogRead(A1);
  double distanceLeftSensor = constrain(valFromIRSensorLeft, 200, 800);
  double distanceRightSensor = constrain(valFromIRSensorRight, 200, 800);

  switch (state) {
    case S_STANDBY:
      Serial.println(state);
      motors.setSpeeds(0, 0);
      waitForButtonAndCountDown();
      state = S_FLIGHT;
    break;

    case S_FLIGHT:
      Serial.println(state);
      Serial.print("Distance is: Left: ");
      Serial.print(distanceLeftSensor);
      Serial.print("Right: ");
      Serial.print(distanceRightSensor);
      Serial.print(" in state: ");
      Serial.println(state);
      delay(10);
/*
      if (distanceLeftSensor < 800) {
        int volume = map(distanceLeftSensor, 125, 0, 3, 15);
        buzzer.playNote(NOTE_G(4), 500, volume); //Volume jglp kdo
      } else {
        motors.setSpeeds(0, 0);
        //motors.setSpeeds(-TURN_SPEED, TURN_SPEED);
      }*/
    break;

    case S_SCOUT:

      // https://acroname.com/articles/linearizing-sharp-ranger-data

      if (sensor_values[0] COLOR_EDGE QTR_THRESHOLD) {
        // if leftmost sensor detects line, reverse and turn to the right
        turn(RIGHT, true);
        //motors.setSpeeds(FORWARD_SPEED, FORWARD_SPEED);
      }
      else if (sensor_values[5] COLOR_EDGE QTR_THRESHOLD) {
        // if rightmost sensor detects line, reverse and turn to the left
        turn(LEFT, true);
        //motors.setSpeeds(FORWARD_SPEED, FORWARD_SPEED);
      }
      else {
        // otherwise, go straight
        //motors.setSpeeds(FORWARD_SPEED, FORWARD_SPEED);
        Serial.print("Distance is: ");
        Serial.print(distanceLeftSensor);
        Serial.print(" in state: ");
        Serial.println(state);
        delay(10);

        if (check_for_contact()) {
          on_contact_made();
        }

        if (distanceLeftSensor < 800) {
          int volume = map(distanceLeftSensor, 125, 0, 3, 15);
          buzzer.playNote(NOTE_G(4), 500, volume); //Volume jglp kdo
          int speed = getForwardSpeed();
          motors.setSpeeds(speed, speed);
        } else {
          motors.setSpeeds(0, 0);
          //motors.setSpeeds(-TURN_SPEED, TURN_SPEED);
        }
      }

    break;
  }
}
