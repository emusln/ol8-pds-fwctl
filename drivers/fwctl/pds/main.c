// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2024 Pensando Systems, Inc */

#include <linux/module.h>
#include <linux/fwctl.h>
#include <linux/auxiliary_bus.h>
#include <linux/pci.h>
#include <uapi/fwctl/pds.h>

#include <linux/pds/pds_common.h>
#include <linux/pds/pds_core_if.h>
#include <linux/pds/pds_adminq.h>
#include <linux/pds/pds_auxbus.h>

struct pdsfc_uctx {
	struct fwctl_uctx uctx;
	u32 uctx_caps;
	u32 uctx_uid;
};

struct pdsfc_dev {
	struct fwctl_device fwctl;
	struct pds_auxiliary_dev *padev;
	struct pdsc *pdsc;
	u32 caps;
};
DEFINE_FREE(pdsfc_dev, struct pdsfc_dev *, if (_T) fwctl_put(&_T->fwctl));

static int pdsfc_open_uctx(struct fwctl_uctx *uctx)
{
	struct pdsfc_dev *pdsfc = container_of(uctx->fwctl, struct pdsfc_dev, fwctl);
	struct pdsfc_uctx *pdsfc_uctx = container_of(uctx, struct pdsfc_uctx, uctx);

	pdsfc_uctx->uctx_caps = pdsfc->caps;

	return 0;
}

static void pdsfc_close_uctx(struct fwctl_uctx *uctx)
{
}

static void *pdsfc_info(struct fwctl_uctx *uctx, size_t *length)
{
	struct pdsfc_uctx *pdsfc_uctx = container_of(uctx, struct pdsfc_uctx, uctx);
	struct fwctl_info_pds *info;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return ERR_PTR(-ENOMEM);

	info->uctx_caps = pdsfc_uctx->uctx_caps;

	return info;
}

static void *pdsfc_fw_rpc(struct fwctl_uctx *uctx, enum fwctl_rpc_scope scope,
			  void *in, size_t in_len, size_t *out_len)
{
	struct pdsfc_dev *pdsfc = container_of(uctx->fwctl, struct pdsfc_dev, fwctl);
	int ret;

	union pds_core_adminq_cmd cmd = {
		.fwctl_rpc.opcode = PDS_AQ_CMD_FWCTL_RPC,
	};
	union pds_core_adminq_comp *resp = NULL;

	if (scope > FWCTL_RPC_DEBUG_READ_ONLY)
		return ERR_PTR(-EPERM);

	/* alloc a return data buffer that fwctl can free */
	resp = kvzalloc(sizeof(*resp), GFP_KERNEL);
	if (!resp)
		return ERR_PTR(-ENOMEM);

	/* copy the incoming request into the adminq request */
	memcpy(cmd.fwctl_rpc.data, in,
	       min(in_len, sizeof(cmd.fwctl_rpc.data)));

	/* send the adminq request */
	ret = pds_client_adminq_cmd(pdsfc->padev, &cmd, sizeof(cmd), resp, 0);
	if (ret)
		return ERR_PTR(ret);
	*out_len = sizeof(*resp);

	/* return a pointer to the allocated response buffer */
	return resp;
}

static const struct fwctl_ops pdsfc_ops = {
	.device_type = FWCTL_DEVICE_TYPE_PDS,
	.uctx_size = sizeof(struct pdsfc_uctx),
	.open_uctx = pdsfc_open_uctx,
	.close_uctx = pdsfc_close_uctx,
	.info = pdsfc_info,
	.fw_rpc = pdsfc_fw_rpc,
};

static int pdsfc_probe(struct auxiliary_device *adev,
			 const struct auxiliary_device_id *id)
{
	struct pdsfc_dev *pdsfc __free(pdsfc_dev);
	struct pds_auxiliary_dev *padev;
	struct device *dev = &adev->dev;
	int ret;

	padev = container_of(adev, struct pds_auxiliary_dev, aux_dev);
	pdsfc = fwctl_alloc_device(&padev->vf_pdev->dev, &pdsfc_ops, struct pdsfc_dev, fwctl);
	if (!pdsfc)
		return -ENOMEM;
	pdsfc->padev = padev;

	ret = fwctl_register(&pdsfc->fwctl);
	if (ret)
		return ret;
	auxiliary_set_drvdata(adev, no_free_ptr(pdsfc));

	dev_info(dev, "Loaded\n");

	return 0;
}

static void pdsfc_remove(struct auxiliary_device *adev)
{
	struct pdsfc_dev *pdsfc  __free(pdsfc_dev) = auxiliary_get_drvdata(adev);
	struct device *dev = &adev->dev;

	fwctl_unregister(&pdsfc->fwctl);

	dev_info(dev, "Removed\n");
}

static const struct auxiliary_device_id pdsfc_id_table[] = {
	{.name = PDS_CORE_DRV_NAME ".fwctl" },
	{}
};
MODULE_DEVICE_TABLE(auxiliary, pdsfc_id_table);

static struct auxiliary_driver pdsfc_driver = {
	.name = "pds_fwctl",
	.probe = pdsfc_probe,
	.remove = pdsfc_remove,
	.id_table = pdsfc_id_table,
};

module_auxiliary_driver(pdsfc_driver);

MODULE_IMPORT_NS(FWCTL);
MODULE_DESCRIPTION("pds fwctl driver");
MODULE_AUTHOR("Shannon Nelson <shannon.nelson@amd.com>");
MODULE_LICENSE("Dual BSD/GPL");
