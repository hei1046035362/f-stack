<?php
/**
 * @author: Dragon
 * 1、确认日志为DEBUG，如果不是，则编辑文件/etc/tgg_gw/tgg_conf.ini，修改bwserver配置项的log_level为DEBUG，重启网关
 * 2、把当前文件放到开发环境的 /data/dragon/GatewayWorker/ 目录下
 * 3、登录hepeng账号(uid=92302787794178048)， 
 * 4、tail -f /var/log/tgg_gateway/gwbwprc* 确认是否登上了新网关(新登录的用户会有一堆的add gid的日志)
 * 5、执行脚本 php client.php
 * 6、在终端查看输出结果是否正常
 */

ini_set('display_errors', 'on');

require_once __DIR__ . '/vendor/autoload.php';

use GatewayClient\Gateway;


// huhu  私信gid：53825030609440768   也是uid
// hepeng 私信gid:92302787794178048   也是uid     发私信的时候是发sendtogroup，但是gid实际上是用的uid

Gateway::$registerAddress = '10.2.1.61:1238';
// $client_id = '0a02013d1f5500000101';
$uid = '92302787794178048';
$uids = ['53825030609440768', '92302787794178048'];
$data = "发送内容部分";
$group = '137946312400039936';// h的  常规
$group1 = '53825030609440768';
$session = json_decode('{"os":"ios","version":"1.2.0","build_number":"57","device_id":"F2E37AD6-8086-4E19-AE4B-3F553CE0A6A5"}',1);
$group_real = '98421291031203840';



// GatewayClient支持GatewayWorker中的所有接口(Gateway::closeCurrentClient Gateway::sendToCurrentClient除外)
Gateway::sendToAll($data);

$client_ids = Gateway::getClientIdByUid($uid);
print_r("getClientIdByUid:");
var_dump($client_ids);
$client_id = $client_ids[0];

Gateway::sendToClient($client_id, $data);

$is_online = Gateway::isOnline($client_id);
print_r("isOnline:");
var_dump($is_online);

Gateway::bindUid($client_id, $uid);

$is_uidonline = Gateway::isUidOnline($uid);
print_r("isUidOnline:");
var_dump( $is_uidonline);

$is_uidsonline = Gateway::isUidsOnline($uids);
print_r("isUidsOnline:");
var_dump($is_uidsonline);

Gateway::sendToUid($uid, $data);

Gateway::joinGroup($client_id, $group);

Gateway::sendToGroup($group, $data);

$cli_count = Gateway::getClientCountByGroup($group);
print_r("getClientCountByGroup:");
var_dump($cli_count);

$cli_sessions = Gateway::getClientSessionsByGroup($group);
print_r("getClientSessionsByGroup:");
var_dump($cli_sessions);

$all_clicount = Gateway::getAllClientCount();
print_r("getAllClientCount:");
var_dump($all_clicount);

$all_clisession = Gateway::getAllClientSessions();
print_r("getAllClientSessions:");
var_dump($all_clisession);

Gateway::setSession($client_id, $session);

Gateway::updateSession($client_id, $session);

$cli_session = Gateway::getSession($client_id);
print_r("getSession:");
var_dump($cli_session);

$uidlist = Gateway::getUidListByGroup($group);
print_r("getUidListByGroup:");
var_dump($uidlist);
//
//
$allgroup = Gateway::getAllGroupIdList();
print_r("getAllGroupIdList:");
var_dump($allgroup);

Gateway::leaveGroup($client_id, $group);
Gateway::unbindUid($client_id, $uid);
Gateway::closeClient($client_id);
