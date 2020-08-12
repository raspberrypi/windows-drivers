#ifndef BCMGENET_REGISTERS_H
#define BCMGENET_REGISTERS_H

#define BG_MAJOR_V5                                 6
#define BG_NUM_BDS                                  256
#define BG_NUM_RINGS                                17
#define BG_DEFAULT_RING                             16
#define BG_MAX_DMA_BURST                            8
#define BG_HFB_FILTER_CNT                           48
#define BG_HFB_FILTER_SIZE                          128

#define BG_SYS_PORT_MODE_EXT_GPHY                   3

#define BG_SYS_RBUF_FLUSH_RESET                     (1<<1)

#define BG_EXT_RGMII_OOB_ID_MODE_DIS                (1<<16)
#define BG_EXT_RGMII_OOB_MODE_EN                    (1<<6)
#define BG_EXT_RGMII_OOB_DISABLE                    (1<<5)
#define BG_EXT_RGMII_OOB_LINK                       (1<<4)

#define BG_INTR_TXDMA_DONE                          (1<<16)
#define BG_INTR_RXDMA_DONE                          (1<<13)

#define BG_RBUF_ALIGN_2B                            (1<<1)

#define BG_UMAC_CMD_TX_PAUSE_IGNORE                 (1<<28)
#define BG_UMAC_CMD_LCL_LOOP_EN                     (1<<15)
#define BG_UMAC_CMD_SW_RESET                        (1<<13)
#define BG_UMAC_CMD_HD_EN                           (1<<10)
#define BG_UMAC_CMD_RX_PAUSE_IGNORE                 (1<<8)
#define BG_UMAC_CMD_PROMISC                         (1<<4)
#define BG_UMAC_CMD_SPEED_1000                      (1<<3)
#define BG_UMAC_CMD_SPEED_100                       (1<<2)
#define BG_UMAC_CMD_RX_EN                           (1<<1)
#define BG_UMAC_CMD_TX_EN                           (1<<0)

#define BG_UMAC_MIB_RESET_TX                        (1<<2)
#define BG_UMAC_MIB_RESET_RUNT                      (1<<1)
#define BG_UMAC_MIB_RESET_RX                        (1<<0)

#define BG_UMAC_MAX_MAC_FILTERS                     17

#define BG_MDIO_START_BUSY                          (1<<29)
#define BG_MDIO_READ                                (1<<27)
#define BG_MDIO_WRITE                               (1<<26)
#define BG_MDIO_ADDR_SHIFT                          21
#define BG_MDIO_REG_SHIFT                           16
#define BG_MDIO_REG_MASK                            0x1f
#define BG_MDIO_DATA_MASK                           0xffff

#define BG_MII_BMCR                                 0x0
#define BG_MII_BMSR                                 0x1
#define BG_MII_PHYSID1                              0x2
#define BG_MII_PHYSID2                              0x3

#define BG_MII_BMCR_RESET                           (1<<15)

#define BG_MII_BMSR_ANCOMP                          (1<<5)
#define BG_MII_BMSR_LINK                            (1<<2)

#define BG_MII_BCM_AUXCTL                           0x18
#define BG_MII_BCM_AUXSTS                           0x19
#define BG_MII_BCM_SHD                              0x1c

#define BG_MII_BCM_AUXCTL_SHD_READ_SHIFT            12
#define BG_MII_BCM_AUXCTL_SHD_MASK                  0x0007

#define BG_MII_BCM_AUXCTL_SHD_MISC                  0x7

#define BG_MII_BCM_AUXCTL_SHD_MISC_WRITE_EN         (1<<15)
#define BG_MII_BCM_AUXCTL_SHD_MISC_RGMII_SKEW_EN    (1<<9)

#define BG_MII_BCM_AUXSTS_ANRESULT                  0x0700

#define BG_MII_BCM_RESULT_1000FD                    0x0700
#define BG_MII_BCM_RESULT_1000HD                    0x0600
#define BG_MII_BCM_RESULT_100FD                     0x0500
#define BG_MII_BCM_RESULT_100T4                     0x0400
#define BG_MII_BCM_RESULT_100HD                     0x0300
#define BG_MII_BCM_RESULT_10FD                      0x0200
#define BG_MII_BCM_RESULT_10HD                      0x0100

#define BG_MII_BCM_SHD_CLK                          0x3

#define BG_MII_BCM_SHD_SEL_SHIFT                    10
#define BG_MII_BCM_SHD_DATA_MASK                    0x03ff
#define BG_MII_BCM_SHD_WRITE_EN                     (1<<15)

#define BG_MII_BCM_SHD_CLK_GTXCLK_EN                (1<<9)

#define BG_PHY_ID_BCM54213PE                        0x600d84a2

#define BG_DMA_BD_LENGTH_SHIFT                      16
#define BG_DMA_BG_STATUS_EOP                        (1<<14)
#define BG_DMA_BG_STATUS_SOP                        (1<<13)
#define BG_DMA_BG_STATUS_TX_QTAG                    (0x3f<<7)
#define BG_DMA_BG_STATUS_TX_CRC                     (1<<6)
#define BG_DMA_BG_STATUS_RX_ERRORS                  (0x1f<<0)

#define BG_DMA_RING_SIZE_SHIFT                      16
#define BG_DMA_EN                                   (1<<0)
#define BG_DMA_RING_BUF_EN_SHIFT                    0x1
#define BG_DMA_RING_XON_XOF_SHIFT                   16

#pragma pack(1)

typedef struct _BG_SYS_REGS
{
    ULONG Rev_Ctrl;                                 /* 0x00 */
    ULONG Port_Ctrl;                                /* 0x04 */
    ULONG RBuf_Flush_Ctrl;                          /* 0x08 */
    ULONG TBuf_Flush_Ctrl;                          /* 0x0c */
} BG_SYS_REGS;                                      /* 0x10 */

static_assert(sizeof(BG_SYS_REGS) == 0x10, "Size of BG_SYS_REGS does not match hardware");
static_assert(FIELD_OFFSET(BG_SYS_REGS, RBuf_Flush_Ctrl) == 0x08, "Wrong RBuf_Flush_Ctrl offset");

typedef struct _BG_EXT_REGS
{
    ULONG Ext_Pwr_Mgmt;                             /* 0x00 */
    ULONG Pad1[2];
    ULONG RGMII_OOB_Ctrl;                           /* 0x0c */
    ULONG Pad2[3];
    ULONG GPHY_Ctrl;                                /* 0x1c */
} BG_EXT_REGS;                                      /* 0x20 */

static_assert(sizeof(BG_EXT_REGS) == 0x20, "Size of BG_EXT_REGS does not match hardware");

typedef struct _BG_INTRL2_REGS
{
    ULONG CPU_Status;                               /* 0x00 */
    ULONG CPU_Set;                                  /* 0x04 */
    ULONG CPU_Clear;                                /* 0x08 */
    ULONG CPU_Mask_Status;                          /* 0x0c */
    ULONG CPU_Mask_Set;                             /* 0x10 */
    ULONG CPU_Mask_Clear;                           /* 0x14 */
} BG_INTRL2_REGS;                                   /* 0x18 */

static_assert(sizeof(BG_INTRL2_REGS) == 0x18, "Size of BG_INTRL2_REGS does not match hardware");

typedef struct _BG_RBUF_REGS
{
    ULONG Ctrl;                                     /* 0x00 */
    ULONG Pad1[2];
    ULONG Status;                                   /* 0x0c */
    ULONG Pad2;
    ULONG Chk_Ctrl;                                 /* 0x14 */
    ULONG Pad3[31];
    ULONG Ovfl_Cnt_V3Plus;                          /* 0x94 */
    ULONG Err_Cnt_V3Plus;                           /* 0x98 */
    ULONG Energy_Ctrl;                              /* 0x9c */
    ULONG Pad4[5];
    ULONG TBuf_Size_Ctrl;                           /* 0xb4 */
} BG_RBUF_REGS;                                     /* 0xb8 */

static_assert(sizeof(BG_RBUF_REGS) == 0xb8, "Size of BG_RBUF_REGS does not match hardware");
static_assert(FIELD_OFFSET(BG_RBUF_REGS, Ctrl) == 0x00, "Wrong Ctrl offset");
static_assert(FIELD_OFFSET(BG_RBUF_REGS, TBuf_Size_Ctrl) == 0xb4, "Wrong TBuf_Size_Ctrl offset");

typedef struct _BG_TBUF_REGS
{
    ULONG Ctrl;                                     /* 0x00 */
    ULONG Pad1[2];
    ULONG BP_MC;                                    /* 0x0c */
    ULONG Pad2;
    ULONG Energy_Ctrl;                              /* 0x14 */
} BG_TBUF_REGS;                                     /* 0x18 */

static_assert(sizeof(BG_TBUF_REGS) == 0x18, "Size of BG_TBUF_REGS does not match hardware");

typedef struct _BG_UMAC_REGS
{
    ULONG Pad1;
    ULONG HD_BKP_Ctrl;                              /* 0x004 */
    ULONG Cmd;                                      /* 0x008 */
    ULONG MAC0;                                     /* 0x00c */
    ULONG MAC1;                                     /* 0x010 */
    ULONG Max_Frame_Len;                            /* 0x014 */
    ULONG Pad2[11];
    ULONG Mode;                                     /* 0x044 */
    ULONG Pad3[7];
    ULONG EEE_Ctrl;                                 /* 0x064 */
    ULONG EEE_LPI_Timer;                            /* 0x068 */
    ULONG EEE_Wake_Timer;                           /* 0x06c */
    ULONG EEE_Ref_Count;                            /* 0x070 */
    ULONG Pad4[176];
    ULONG Tx_Flush;                                 /* 0x334 */
    ULONG Pad5[50];
    ULONG MIB_Start;                                /* 0x400 */
    ULONG Pad6[95];
    ULONG MIB_Ctrl;                                 /* 0x580 */
    ULONG Pad7[36];
    ULONG MDIO_Cmd;                                 /* 0x614 */
    ULONG MDIO_Cfg;                                 /* 0x618 */
    ULONG Pad8;
    ULONG MPD_Ctrl;                                 /* 0x620 */
    ULONG MPD_PW_MS;                                /* 0x624 */
    ULONG MPD_PW_LS;                                /* 0x628 */
    ULONG Pad9[3];
    ULONG MDF_Err_Cnt;                              /* 0x638 */
    ULONG Pad10[5];
    ULONG MDF_Ctrl;                                 /* 0x650 */
    ULONG MDF_Addr[BG_UMAC_MAX_MAC_FILTERS * 2];    /* 0x654 */
} BG_UMAC_REGS;                                     /* 0x6dc */

static_assert(sizeof(BG_UMAC_REGS) == 0x6dc, "Size of BG_UMAC_REGS does not match hardware");
static_assert(FIELD_OFFSET(BG_UMAC_REGS, Cmd) == 0x008, "Wrong Cmd offset");
static_assert(FIELD_OFFSET(BG_UMAC_REGS, Max_Frame_Len) == 0x014, "Wrong Cmd offset");
static_assert(FIELD_OFFSET(BG_UMAC_REGS, MIB_Ctrl) == 0x580, "Wrong MIB_Ctrl offset");

typedef struct _BG_DMA_DESC
{
    ULONG Length_Status;                            /* 0x0 */
    ULONG Address_Lo;                               /* 0x4 */
    ULONG Address_Hi;                               /* 0x8 */
} BG_DMA_DESC;                                      /* 0xc */

static_assert(sizeof(BG_DMA_DESC) == 0xc, "Size of BG_DMA_DESC does not match hardware");

typedef struct _BG_DMA_RING_REGS_V4
{
    ULONG TDMA_Read_Ptr;                            /* 0x00 - RDMA write */
    ULONG TDMA_Read_Ptr_Hi;                         /* 0x04 */
    ULONG TDMA_Cons_Index;                          /* 0x08 - RDMA prod */
    ULONG TDMA_Prod_Index;                          /* 0x0c - RDMA cons */
    ULONG DMA_Ring_Buf_Size;                        /* 0x10 */
    ULONG DMA_Start_Addr;                           /* 0x14 */
    ULONG DMA_Start_Addr_Hi;                        /* 0x18 */
    ULONG DMA_End_Addr;                             /* 0x1c */
    ULONG DMA_End_Addr_Hi;                          /* 0x20 */
    ULONG DMA_MBuf_Done_Thresh;                     /* 0x24 */
    ULONG TDMA_Flow_Period;                         /* 0x28 - RDMA XON/XOF threshold */
    ULONG TDMA_Write_Ptr;                           /* 0x2c - RDMA read */
    ULONG TDMA_Write_Ptr_Hi;                        /* 0x30 */
    ULONG Pad1[3];
} BG_DMA_RING_REGS_V4;                              /* 0x40 */

static_assert(sizeof(BG_DMA_RING_REGS_V4) == 0x40, "Size of BG_DMA_RING_REGS_V4 does not match hardware");

typedef struct _BG_DMA_REGS_V3PLUS
{
    ULONG Ring_Cfg;                                 /* 0x00 */
    ULONG Ctrl;                                     /* 0x04 */
    ULONG Status;                                   /* 0x08 */
    ULONG SCB_Burst_Size;                           /* 0x0c */
    ULONG Pad1[7];
    ULONG ARB_Ctrl_Ring0_Timeout;                   /* 0x2c */
    ULONG Priority_0_Ring1_Timeout;                 /* 0x30 */
    ULONG Priority_1_Ring2_Timeout;                 /* 0x34 */
    ULONG Priority_2_Ring3_Timeout;                 /* 0x38 */
    ULONG Ring4_Timeout;                            /* 0x3c */
    ULONG Ring5_Timeout;                            /* 0x40 */
    ULONG Ring6_Timeout;                            /* 0x44 */
    ULONG Ring7_Timeout;                            /* 0x48 */
    ULONG Ring8_Timeout;                            /* 0x4c */
    ULONG Ring9_Timeout;                            /* 0x50 */
    ULONG Ring10_Timeout;                           /* 0x54 */
    ULONG Ring11_Timeout;                           /* 0x58 */
    ULONG Ring12_Timeout;                           /* 0x5c */
    ULONG Ring13_Timeout;                           /* 0x60 */
    ULONG Ring14_Timeout;                           /* 0x64 */
    ULONG Ring15_Timeout;                           /* 0x68 */
    ULONG Ring16_Timeout;                           /* 0x6c */
    ULONG Index2Ring_0;                             /* 0x70 */
    ULONG Index2Ring_1;                             /* 0x74 */
    ULONG Index2Ring_2;                             /* 0x78 */
    ULONG Index2Ring_3;                             /* 0x7c */
    ULONG Index2Ring_4;                             /* 0x80 */
    ULONG Index2Ring_5;                             /* 0x84 */
    ULONG Index2Ring_6;                             /* 0x88 */
    ULONG Index2Ring_7;                             /* 0x8c */
} BG_DMA_REGS_V3PLUS;                               /* 0x90 */

static_assert(sizeof(BG_DMA_REGS_V3PLUS) == 0x90, "Size of BG_DMA_REGS_V3PLUS does not match hardware");

typedef struct _BG_DMA
{
    BG_DMA_DESC BDs[BG_NUM_BDS];                    /* 0x0000 */
    BG_DMA_RING_REGS_V4 Rings[BG_NUM_RINGS];        /* 0x0c00 */
    BG_DMA_REGS_V3PLUS Regs;                        /* 0x1040 */
} BG_DMA;                                           /* 0x10d0 */

static_assert(sizeof(BG_DMA) == 0x10d0, "Size of BG_DMA does not match hardware");

typedef struct _BG_HFB_REGS
{
    ULONG Ctrl;                                     /* 0x00 */
    ULONG Flt_Enable_V3Plus[2];                     /* 0x04 */
    ULONG Pad1[4];
    ULONG Flt_Len_V3Plus[BG_HFB_FILTER_CNT/4];      /* 0x1c */
} BG_HFB_REGS;                                      /* 0x4c */

static_assert(sizeof(BG_HFB_REGS) == 0x4c, "Size of BG_HFB_REGS does not match hardware");

typedef struct _BG_HFB_FILTER
{
    ULONG Data[BG_HFB_FILTER_SIZE];                 /* 0x000 */
} BG_HFB_FILTER;                                    /* 0x200 */

static_assert(sizeof(BG_HFB_FILTER) == 0x200, "Size of BG_HFB_FILTER does not match hardware");

typedef struct GenetRegisters
{
    BG_SYS_REGS Sys;                                /* 0x0000 */
    ULONG Pad1[28];
    BG_EXT_REGS Ext;                                /* 0x0080 */
    ULONG Pad2[88];
    BG_INTRL2_REGS INTRL2_0;                        /* 0x0200 */
    ULONG Pad3[10];
    BG_INTRL2_REGS INTRL2_1;                        /* 0x0240 */
    ULONG Pad4[42];
    BG_RBUF_REGS RBuf;                              /* 0x0300 */
    ULONG Pad5[146];
    BG_TBUF_REGS TBuf;                              /* 0x0600 */
    ULONG Pad6[122];
    BG_UMAC_REGS UMAC;                              /* 0x0800 */
    ULONG Pad7[1097];
    BG_DMA RDMA;                                    /* 0x2000 */
    ULONG Pad8[972];
    BG_DMA TDMA;                                    /* 0x4000 */
    ULONG Pad9[3020];
    BG_HFB_FILTER HFB_Filters[BG_HFB_FILTER_CNT];   /* 0x8000 */
    ULONG Pad10[1792];
    BG_HFB_REGS HFB;                                /* 0xfc00 */
    ULONG Pad11[237];
} GenetRegisters;                                /* 0x10000 */

static_assert(sizeof(GenetRegisters) == 0x10000, "Size of BG_REGS does not match hardware");
static_assert(FIELD_OFFSET(GenetRegisters, Sys) == 0x0000, "Wrong Sys offset");
static_assert(FIELD_OFFSET(GenetRegisters, Ext) == 0x0080, "Wrong Ext offset");
static_assert(FIELD_OFFSET(GenetRegisters, INTRL2_0) == 0x0200, "Wrong INTRL2_0 offset");
static_assert(FIELD_OFFSET(GenetRegisters, INTRL2_1) == 0x0240, "Wrong INTRL2_1 offset");
static_assert(FIELD_OFFSET(GenetRegisters, RBuf) == 0x0300, "Wrong RBuf offset");
static_assert(FIELD_OFFSET(GenetRegisters, TBuf) == 0x0600, "Wrong TBuf offset");
static_assert(FIELD_OFFSET(GenetRegisters, UMAC) == 0x0800, "Wrong UMAC offset");
static_assert(FIELD_OFFSET(GenetRegisters, RDMA) == 0x2000, "Wrong RDMA offset");
static_assert(FIELD_OFFSET(GenetRegisters, TDMA) == 0x4000, "Wrong TDMA offset");
static_assert(FIELD_OFFSET(GenetRegisters, HFB_Filters) == 0x8000, "Wrong HFB_Filters offset");
static_assert(FIELD_OFFSET(GenetRegisters, HFB) == 0xfc00, "Wrong HFB offset");

#pragma pack()

#endif /* BCMGENET_REGISTERS_H */
