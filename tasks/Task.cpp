/* Generated from orogen/lib/orogen/templates/tasks/Task.cpp */

#include "Task.hpp"

using namespace uwmodem_simulation;
using namespace usbl_evologics;

Task::Task(std::string const& name)
    : TaskBase(name), generator(base::Time::now().toMicroseconds())
{
}

Task::Task(std::string const& name, RTT::ExecutionEngine* engine)
    : TaskBase(name, engine), generator(base::Time::now().toMicroseconds())
{
}

Task::~Task()
{
}



/// The following lines are template definitions for the various state machine
// hooks defined by Orocos::RTT. See Task.hpp for more detailed
// documentation about them.

bool Task::configureHook()
{
    if (! TaskBase::configureHook())
        return false;

    last_probability = _probability.get();
    dist = boost::random::bernoulli_distribution<>(last_probability);
    im_retry = _im_retry.get();
    raw_bitrate = _bitrate.get();
    buffer_size = 0;
    interface = _receiver_interface.get();
    im_status.first.status = EMPTY;
    im_status.first.time = base::Time::fromSeconds(0);
    im_status.second = base::Time::now();
    last_write_raw_packet = base::Time::fromSeconds(0);
    status_period = base::Time::fromSeconds(1);
    // Initial position at origin for both devices.
    local_position.position = base::Vector3d::Zero();
    remote_position.position = base::Vector3d::Zero();

    return true;
}
bool Task::startHook()
{
    if (! TaskBase::startHook())
        return false;
    return true;
}
void Task::updateHook()
{
    TaskBase::updateHook();

    /**
     * Output status
     */
    if((base::Time::now() - last_status) > status_period)
    {
        last_status = base::Time::now();
        // Keep delivery time in im_status
        MessageStatus status_out = im_status.first;
        status_out.time = base::Time::now();
        _message_status.write(status_out);
    }

    /**
     * Transmission of Raw Data
     */
    iodrivers_base::RawPacket raw_data_input;
    while(_raw_data_input.read(raw_data_input) == RTT::NewData)
        buffer_size += handleRawPacket(raw_data_input, buffer_size);
    while(checkRawDataTransmission())
        buffer_size -= sendOnePacket();
    while(hasRawData(queueRawPacketOnTheWay))
    {
        _raw_data_output.write(updateSampleTime(queueRawPacketOnTheWay.front().first, queueRawPacketOnTheWay.front().second));
        queueRawPacketOnTheWay.pop();
        // In usbl, it provides a pose when raw data is transmitted. TODO check periodicty of output
        usblPosition(local_position, remote_position);
    }

    /**
     * Transmission of Instant Message
     */
    SendIM send_im;
    while(_message_input.read(send_im) == RTT::NewData)
        enqueueSendIM(send_im);
    while(checkIMStatus(im_status.first))
    {
        im_attempts = defineAttempts();
        im_status = std::make_pair (processIM(im_status.first), base::Time::now());
    }
    DeliveryStatus delivery_status;
    while( (delivery_status = checkDelivery(im_status, im_attempts)) != PENDING
            && im_status.first.status == PENDING)
        im_status = std::make_pair (updateDelivery(im_status, delivery_status), base::Time::now());

    /**
     * Handle Position
     */
    bool new_pose = false;
    base::samples::RigidBodyState pose;
    while(_local_position.read(pose) == RTT::NewData)
    {
        if(pose.hasValidPosition())
        {
            local_position = pose;
            new_pose = true;
        }
    }

   while(_remote_position.read(pose) == RTT::NewData)
    {
        if(pose.hasValidPosition())
        {
            remote_position = pose;
            new_pose = true;
        }
    }

   if(new_pose)
   {
        travel_time = updateTravelTime(local_position, remote_position);
        double prob = updateProbability(local_position, remote_position);
        if(last_probability != prob)
        {
            dist = boost::random::bernoulli_distribution<>(prob);
            last_probability = prob;
        }
   }
}
void Task::errorHook()
{
    TaskBase::errorHook();
}
void Task::stopHook()
{
    TaskBase::stopHook();
}
void Task::cleanupHook()
{
    TaskBase::cleanupHook();
    queueRawPacket = std::queue< std::pair <iodrivers_base::RawPacket, base::Time> >();
    queueSendIM = std::queue< std::pair <usbl_evologics::SendIM, base::Time> >();
    // queueRawPacketOnTheWay is not empty.
    // Well, it's on the way, the receiver must deal with this data. ¯\_(ツ)_/¯
    buffer_size = 0;
    im_status.first.status = EMPTY;
    last_write_raw_packet = base::Time::fromSeconds(0);
}


std::vector<iodrivers_base::RawPacket> Task::breakInTinyPackets(const iodrivers_base::RawPacket &input_data, int packet_size)
{
    std::vector<iodrivers_base::RawPacket> output_vector;
    iodrivers_base::RawPacket output_raw = input_data;
    for( size_t i=0; i<input_data.data.size(); i+=packet_size)
    {
        output_raw.data.clear();
        if(input_data.data.size() - i < packet_size)
            output_raw.data = std::vector<uint8_t>(input_data.data.begin()+i, input_data.data.end());
        else
            output_raw.data = std::vector<uint8_t>(input_data.data.begin()+i, input_data.data.begin()+i+packet_size);
        output_vector.push_back(output_raw);
    }
    return output_vector;
}

int Task::handleRawPacket(const iodrivers_base::RawPacket &input_data, int buffer_size)
{
    iodrivers_base::RawPacket raw_data_input = input_data;
    int increment = 0;
    std::vector<iodrivers_base::RawPacket> tiny_packets;
    tiny_packets = breakInTinyPackets(raw_data_input, (interface == SERIAL)? 2 : packet_size);
    for(size_t i=0; i<tiny_packets.size(); i++)
    {   // Include in buffer as much bytes as possible
        if(buffer_size+increment >= transmission_buffer_size)
            continue;
        increment += tiny_packets[i].data.size();
        queueRawPacket.push(std::make_pair( tiny_packets[i], base::Time::now()));
    }
    return increment;
}

bool Task::checkRawDataTransmission(void)
{
    if(queueRawPacket.empty())
        return false;
    if(last_write_raw_packet.toSeconds() == 0)
        return true;
    if(!controlBitRate(queueRawPacket.front().first.data, last_write_raw_packet, raw_bitrate))
        return false;
    return true;
}

int Task::sendOnePacket(void)
{
    if(queueRawPacket.empty())
        return 0;
    // Take in account waintg time in queue
    std::pair <iodrivers_base::RawPacket, base::Time> on_the_way =
    std::make_pair (updateSampleTime(queueRawPacket.front().first, queueRawPacket.front().second), base::Time::now());
    last_write_raw_packet = on_the_way.second;
    // Don't lose packet
   if(dist(generator))
        queueRawPacketOnTheWay.push(on_the_way);
    queueRawPacket.pop();
    return on_the_way.first.data.size();
}

bool Task::checkTravelTime(base::Time init_time, base::Time travel_time)
{
    return ((base::Time::now() - init_time) >= travel_time);
}

bool Task::controlBitRate(const std::vector<uint8_t> &data, base::Time last_time, int bitrate)
{
    double time_window = (base::Time::now() - last_time).toSeconds();
    return (data.size()*8/time_window <= bitrate);
}

bool Task::hasRawData(const std::queue< std::pair <iodrivers_base::RawPacket, base::Time> > &queueRawData)
{
    if(queueRawData.empty())
        return false;
    if(!checkTravelTime(queueRawData.front().second, travel_time))
        return false;
    return true;
}

bool Task::checkIMStatus(const MessageStatus &im_status)
{
    if(im_status.status == PENDING)
        return false;
    if(queueSendIM.empty())
        return false;
    return true;
}

void Task::enqueueSendIM(const SendIM &send_im)
{
    // Check size of Message. It can't be bigger than MAX_MSG_SIZE, according device's manual.
    if(send_im.buffer.size() > MAX_MSG_SIZE)
    {
        if(state() != HUGE_INSTANT_MESSAGE)
            state(HUGE_INSTANT_MESSAGE);
        RTT::log(RTT::Error) << "Usbl_evologics Task.cpp. HUGE_INSTANT_MESSAGE. Message discharged: \""
                             << UsblParser::printBuffer(send_im.buffer)
                             << "\"" << std::endl;
        return;
    }
    // Check queue size.
    if(queueSendIM.size() > MAX_QUEUE_MSG_SIZE)
    {
        if(state() != FULL_IM_QUEUE)
            state(FULL_IM_QUEUE);
        RTT::log(RTT::Error) << "Usbl_evologics Task.cpp. FULL_IM_QUEUE. Message discharged: \""
                             << UsblParser::printBuffer(send_im.buffer)
                             << "\"" << std::endl;
        return;
    }
    if(state() != RUNNING)
        state(RUNNING);
    queueSendIM.push(std::make_pair (send_im, base::Time::now()));
}

MessageStatus Task::processIM(const MessageStatus &im_status)
{
    if(queueSendIM.empty())
        throw std::runtime_error("uwmodem_simulation::Task.cpp: processIM: queueSendIM is empty");
    MessageStatus status = im_status;
    status.sendIm = queueSendIM.front().first;
    status.status = PENDING;
    // Used to control the transmission time.
    status.time = status.sendIm.time;
    // Update timestamp with wainting time in queue
    status = updateSampleTime(status, queueSendIM.front().second);
    status.messageSent++;
    queueSendIM.pop();
    return status;
}

int Task::defineAttempts(void)
{
    int attempts = 1;
    while (dist(generator) && (attempts <= im_retry))
        attempts++;
    return attempts;
}

DeliveryStatus Task::checkDelivery(const std::pair <MessageStatus, base::Time> &im_status, int attempts)
{
    if(im_status.first.status != PENDING)
        return im_status.first.status;
    if(!controlBitRate(im_status.first.sendIm.buffer, im_status.second, im_bitrate))
        return PENDING;
    int path = im_status.first.sendIm.deliveryReport? (attempts*2) : 1;
    if(!checkTravelTime(im_status.second, travel_time*path))
        return PENDING;
    // Lose Instant Message in last try
    if(!dist(generator) && attempts == im_retry+1)
        return FAILED;
    return DELIVERED;
}

MessageStatus Task::updateDelivery(const std::pair<usbl_evologics::MessageStatus, base::Time> &im_status, const DeliveryStatus &delivery)
{
    MessageStatus status = im_status.first;
    if(delivery == DELIVERED)
    {
        SendIM message_input = im_status.first.sendIm;
        // Take in account the time in queue
        message_input.time = im_status.first.time;
        _message_output.write(toReceivedIM(message_input, im_status.second));
        status.messageDelivered++;
        // In usbl, it provides a pose when a DELIVERED is received and there is no raw data
        if(queueRawPacketOnTheWay.empty())
            usblPosition(local_position, remote_position);
    }
    if(delivery == FAILED)
        status.messageFailed++;

    status.status = delivery;
    status.time = base::Time::now();
    _message_status.write(status);

    if(delivery == DELIVERED)
        status.status = EMPTY;
    status.sendIm = SendIM();
    return status;
}

ReceiveIM Task::toReceivedIM(const SendIM &send_im, base::Time start_delivery)
{
    ReceiveIM msg;
    msg.time = send_im.time;
    msg = updateSampleTime(msg, start_delivery);
    msg.duration = msg.time - send_im.time;
    msg.buffer = send_im.buffer;
    msg.destination = send_im.destination;
    msg.source = (send_im.destination == 1)? 2 : 1;
    msg.deliveryReport = send_im.deliveryReport;
    msg.rssi = -(rand() % 65 + 20);
    msg.integrity = rand() % 60 + 90;
    msg.velocity = (rand() % 1000)/1000;
    return msg;
}

base::Time Task::updateTravelTime(const base::samples::RigidBodyState &local, const base::samples::RigidBodyState &remote)
{
    double distance = (local_position.position - remote_position.position).norm();
    return base::Time::fromSeconds(distance/sound_velocity);
}

double Task::updateProbability(const base::samples::RigidBodyState &local, const base::samples::RigidBodyState &remote)
{
    // Out of water
    if(local.position[2] > 0 || remote.position[2] > 0)
        return 0;
    // Out of range
    double distance = (local_position.position - remote_position.position).norm();
   if(distance > max_distance)
       return 0;
   return _probability.get();
}
