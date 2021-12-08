#ifndef __PMIC_GLINK_H__
#define __PMIC_GLINK_H__

struct pmic_glink;

struct pmic_glink_hdr {
	__le32 owner;
	__le32 type;
	__le32 opcode;
};

int pmic_glink_send(struct pmic_glink *pmic, void *data, size_t len);


struct pmic_glink_owner;

struct pmic_glink_owner *pmic_glink_register_callback(struct pmic_glink *pg,
						      unsigned int id,
						      void (*cb)(const void *, size_t, void *),
						      void *priv);

void pmic_glink_unregister_callback(struct pmic_glink *pg,
				    struct pmic_glink_owner *owner);


#endif
