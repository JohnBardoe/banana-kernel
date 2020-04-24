#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/soc/qcom/apr.h>
#include "q6mvm.h"
#include "q6voice-common.h"
#include "q6voice-downstream.h"

struct q6voice_session *q6mvm_session_create(enum q6voice_path_type path)
{
	struct mvm_create_ctl_session_cmd cmd;

	cmd.hdr.pkt_size = sizeof(cmd);
	cmd.hdr.opcode = VSS_IMVM_CMD_CREATE_PASSIVE_CONTROL_SESSION;

	strlcpy(cmd.mvm_session.name, "default modem voice",
		strlen("default modem voice")+1);

	return q6voice_session_create(Q6VOICE_SERVICE_MVM, path, &cmd.hdr);
}

int q6mvm_set_dual_control(struct q6voice_session *mvm)
{
	struct mvm_modem_dual_control_session_cmd cmd;

	dev_info(mvm->dev, "set dual control\n");

	cmd.hdr.pkt_size = sizeof(cmd);
	cmd.hdr.opcode = VSS_IMVM_CMD_SET_POLICY_DUAL_CONTROL;

	cmd.voice_ctl.enable_flag = true;

	return q6voice_common_send(mvm, &cmd.hdr);
}

int q6mvm_attach(struct q6voice_session *mvm, struct q6voice_session *cvp)
{
	struct mvm_attach_vocproc_cmd cmd;

	dev_info(mvm->dev, "attach vocproc: %d\n", cvp->handle);

	cmd.hdr.pkt_size = sizeof(cmd);
	cmd.hdr.opcode = VSS_IMVM_CMD_ATTACH_VOCPROC;

	cmd.mvm_attach_cvp_handle.handle = cvp->handle;

	return q6voice_common_send(mvm, &cmd.hdr);
}

int q6mvm_detach(struct q6voice_session *mvm, struct q6voice_session *cvp)
{
	struct mvm_detach_vocproc_cmd cmd;

	dev_info(mvm->dev, "detach vocproc: %d\n", cvp->handle);

	cmd.hdr.pkt_size = sizeof(cmd);
	cmd.hdr.opcode = VSS_IMVM_CMD_DETACH_VOCPROC;

	cmd.mvm_detach_cvp_handle.handle = cvp->handle;

	return q6voice_common_send(mvm, &cmd.hdr);
}

int q6mvm_start(struct q6voice_session *mvm)
{
	struct apr_pkt cmd;

	dev_info(mvm->dev, "start\n");

	cmd.hdr.pkt_size = APR_HDR_SIZE;
	cmd.hdr.opcode = VSS_IMVM_CMD_START_VOICE;

	return q6voice_common_send(mvm, &cmd.hdr);
}

int q6mvm_stop(struct q6voice_session *mvm)
{
	struct apr_pkt cmd;

	dev_info(mvm->dev, "stop\n");

	cmd.hdr.pkt_size = APR_HDR_SIZE;
	cmd.hdr.opcode = VSS_IMVM_CMD_STOP_VOICE;

	return q6voice_common_send(mvm, &cmd.hdr);
}

static int q6mvm_probe(struct apr_device *adev)
{
	int ret;

	ret = q6voice_common_probe(adev, Q6VOICE_SERVICE_MVM);
	if (ret)
		return ret;

	return of_platform_populate(adev->dev.of_node, NULL, NULL, &adev->dev);
}

static int q6mvm_remove(struct apr_device *adev)
{
	of_platform_depopulate(&adev->dev);
	return q6voice_common_remove(adev);
}

static const struct of_device_id q6mvm_device_id[]  = {
	{ .compatible = "qcom,q6mvm" },
	{},
};
MODULE_DEVICE_TABLE(of, q6mvm_device_id);

static struct apr_driver qcom_q6mvm_driver = {
	.probe = q6mvm_probe,
	.remove = q6mvm_remove,
	.callback = q6voice_common_callback,
	.driver = {
		.name = "qcom-q6mvm",
		.of_match_table = of_match_ptr(q6mvm_device_id),
	},
};

module_apr_driver(qcom_q6mvm_driver);
MODULE_DESCRIPTION("Q6 Multimode Voice Manager");
MODULE_LICENSE("GPL v2");
