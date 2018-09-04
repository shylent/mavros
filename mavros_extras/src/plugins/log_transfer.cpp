#include <stack>

#include <mavros/mavros_plugin.h>
#include <mavros_msgs/LogData.h>
#include <mavros_msgs/LogEntry.h>
#include <mavros_msgs/LogRequestData.h>
#include <mavros_msgs/LogRequestEnd.h>
#include <mavros_msgs/LogRequestList.h>
#include <mavros_msgs/GetLogListAction.h>
#include <mavros_msgs/LogListItem.h>
#include <mavros_msgs/GetLogAction.h>

#include <actionlib/server/simple_action_server.h>

namespace mavros {
namespace extra_plugins {
class LogTransferPlugin : public plugin::PluginBase {
private:
	using GetLogListActionServer = actionlib::SimpleActionServer<mavros_msgs::GetLogListAction>;
	GetLogListActionServer get_log_list_action_srv;

	using GetLogActionServer = actionlib::SimpleActionServer<mavros_msgs::GetLogAction>;
	GetLogActionServer get_log_action_srv;

public:
	LogTransferPlugin() :
		nh("~log_transfer"),
		get_log_list_action_srv(nh, "get_log_list", false),
		get_log_action_srv(nh, "get_log", false) {}

	void initialize(UAS& uas) override;

	Subscriptions get_subscriptions() override;

private:
	ros::NodeHandle nh;
	ros::Publisher log_entry_pub, log_data_pub;
	ros::ServiceServer log_request_list_srv, log_request_data_srv, log_request_end_srv;

	enum class State {
		Idle,
		GettingLogList,
		GettingLogData,
	};
	State state = State::Idle;

	std::stack<mavros_msgs::LogListItem> logListItems;

	void handle_log_entry(const mavlink::mavlink_message_t *msg, mavlink::common::msg::LOG_ENTRY& le);
	void handle_log_data(const mavlink::mavlink_message_t *msg, mavlink::common::msg::LOG_DATA& ld);
	bool log_request_list_cb(mavros_msgs::LogRequestList::Request &req,
				mavros_msgs::LogRequestList::Response &res);
	bool log_request_data_cb(mavros_msgs::LogRequestData::Request &req,
				mavros_msgs::LogRequestData::Response &res);
	bool log_request_end_cb(mavros_msgs::LogRequestEnd::Request &req,
				mavros_msgs::LogRequestEnd::Response &res);

	bool stopLogTransfer();

	void logListRequested();
	void logListRequestPreempted();
	void logRequested();
	void logRequestPreempted();

	static constexpr char* logName = "log_transfer";
};

void LogTransferPlugin::initialize(UAS& uas)
{
	PluginBase::initialize(uas);

	log_entry_pub = nh.advertise<mavros_msgs::LogEntry>("raw/log_entry", 1000);
	log_data_pub = nh.advertise<mavros_msgs::LogData>("raw/log_data", 1000);

	log_request_list_srv = nh.advertiseService("raw/log_request_list",
				&LogTransferPlugin::log_request_list_cb, this);
	log_request_data_srv = nh.advertiseService("raw/log_request_data",
				&LogTransferPlugin::log_request_data_cb, this);
	log_request_end_srv = nh.advertiseService("raw/log_request_end",
				&LogTransferPlugin::log_request_end_cb, this);

	get_log_list_action_srv.registerGoalCallback(boost::bind(&LogTransferPlugin::logListRequested, this));
	get_log_list_action_srv.registerPreemptCallback(boost::bind(&LogTransferPlugin::logListRequestPreempted, this));
	get_log_list_action_srv.start();

	get_log_action_srv.registerGoalCallback(boost::bind(&LogTransferPlugin::logRequested, this));
	get_log_action_srv.registerPreemptCallback(boost::bind(&LogTransferPlugin::logRequestPreempted, this));
	get_log_action_srv.start();
}

plugin::PluginBase::Subscriptions LogTransferPlugin::get_subscriptions()
{
	return {
		       make_handler(&LogTransferPlugin::handle_log_entry),
		       make_handler(&LogTransferPlugin::handle_log_data),
	};
}

void LogTransferPlugin::handle_log_entry(const mavlink::mavlink_message_t*,
			mavlink::common::msg::LOG_ENTRY& le)
{
	auto msg = boost::make_shared<mavros_msgs::LogEntry>();
	msg->header.stamp = ros::Time::now();
	msg->id = le.id;
	msg->num_logs = le.num_logs;
	msg->last_log_num = le.last_log_num;
	msg->time_utc = ros::Time(le.time_utc);
	msg->size = le.size;
	log_entry_pub.publish(msg);
}

void LogTransferPlugin::handle_log_data(const mavlink::mavlink_message_t*,
			mavlink::common::msg::LOG_DATA& ld)
{
	auto msg = boost::make_shared<mavros_msgs::LogData>();
	msg->header.stamp = ros::Time::now();
	msg->id = ld.id;
	msg->offset = ld.ofs;

	auto count = ld.count;
	if (count > ld.data.max_size()) {
		count = ld.data.max_size();
	}
	msg->data.insert(msg->data.cbegin(), ld.data.cbegin(), ld.data.cbegin() + count);
	log_data_pub.publish(msg);
}

bool LogTransferPlugin::log_request_list_cb(mavros_msgs::LogRequestList::Request& req,
			mavros_msgs::LogRequestList::Response& res)
{
	mavlink::common::msg::LOG_REQUEST_LIST msg;
	m_uas->msg_set_target(msg);
	msg.start = req.start;
	msg.end = req.end;

	res.success = true;
	try {
		UAS_FCU(m_uas)->send_message(msg);
	} catch (std::length_error&) {
		res.success = false;
	}
	return true;
}

bool LogTransferPlugin::log_request_data_cb(mavros_msgs::LogRequestData::Request& req,
			mavros_msgs::LogRequestData::Response& res)
{
	mavlink::common::msg::LOG_REQUEST_DATA msg;
	m_uas->msg_set_target(msg);
	msg.id = req.id;
	msg.ofs = req.offset;
	msg.count = req.count;

	res.success = true;
	try {
		UAS_FCU(m_uas)->send_message(msg);
	} catch (std::length_error&) {
		res.success = false;
	}
	return true;
}

bool LogTransferPlugin::log_request_end_cb(mavros_msgs::LogRequestEnd::Request&,
			mavros_msgs::LogRequestEnd::Response& res)
{
	res.success = stopLogTransfer();
	return true;
}

bool LogTransferPlugin::stopLogTransfer()
{
	mavlink::common::msg::LOG_REQUEST_END msg;
	m_uas->msg_set_target(msg);
	try {
		UAS_FCU(m_uas)->send_message(msg);
	} catch (std::length_error&) {
		ROS_ERROR_NAMED(logName, "Failed to send LOG_REQUEST_END message");
		return false;
	}
	return true;
}

void LogTransferPlugin::logListRequested()
{
	auto goal = get_log_list_action_srv.acceptNewGoal();
	if (goal == nullptr) {
		ROS_WARN_NAMED(logName, "Null goal received for get_log_list");
		return;
	}

	stopLogTransfer();
	if (get_log_action_srv.isActive()) {
		mavros_msgs::GetLogResult res;
		res.success = false;
		get_log_action_srv.setPreempted(res, "Log list was requested");
	}
	state = State::Idle;
	if (get_log_list_action_srv.isPreemptRequested()) {
		mavros_msgs::GetLogListResult res;
		res.success = false;
		get_log_list_action_srv.setPreempted(res, "Log list request was preempted");
		return;
	}

}

void LogTransferPlugin::logListRequestPreempted()
{

}

void LogTransferPlugin::logRequested()
{
	auto goal = get_log_action_srv.acceptNewGoal();
	if (get_log_list_action_srv.isPreemptRequested()) {
		mavros_msgs::GetLogListResult res;
		res.success = false;
		get_log_list_action_srv.setPreempted(res, "Another log list request was accepted");
		if (get_log_action_srv.isActive()) {
			mavros_msgs::GetLogResult res;
			res.success = false;
			get_log_action_srv.setPreempted(res, "Log list was requested");
		}
		return;
	}
}

void LogTransferPlugin::logRequestPreempted()
{}
}	// namespace extra_plugins
}	// namespace mavros

#include <pluginlib/class_list_macros.h>
PLUGINLIB_EXPORT_CLASS(mavros::extra_plugins::LogTransferPlugin, mavros::plugin::PluginBase)
