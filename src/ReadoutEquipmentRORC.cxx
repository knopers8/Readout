#include "ReadoutEquipment.h"

#include <ReadoutCard/Parameters.h>
#include <ReadoutCard/ChannelFactory.h>
#include <ReadoutCard/MemoryMappedFile.h>
#include <ReadoutCard/DmaChannelInterface.h>
#include <ReadoutCard/Exception.h>
#include <ReadoutCard/Driver.h>

#include <string>
#include <mutex>

#include <Common/Timer.h>

#include "ReadoutUtils.h"
#include "RdhUtils.h"

#include <InfoLogger/InfoLogger.hxx>
using namespace AliceO2::InfoLogger;
extern InfoLogger theLog;



class ReadoutEquipmentRORC : public ReadoutEquipment {

  public:
    ReadoutEquipmentRORC(ConfigFile &cfg, std::string name="rorcReadout");
    ~ReadoutEquipmentRORC();

    Thread::CallbackResult prepareBlocks();
    DataBlockContainerReference getNextBlock(); 
  
  private:
    Thread::CallbackResult  populateFifoOut(); // the data readout loop function
    
    AliceO2::roc::ChannelFactory::DmaChannelSharedPtr channel;    // channel to ROC device

    DataBlockId currentId=0;    // current data id, kept for auto-increment
       
    bool isInitialized=false;     // flag set to 1 when class has been successfully initialized
    bool isWaitingFirstLoop=true;  // flag set until first readout loop called

    int RocFifoSize=0;  // detected size of ROC fifo (when filling it for the first time)

    int cfgRdhCheckEnabled=0; // flag to enable RDH check at runtime
    int cfgRdhDumpEnabled=0;  // flag to enable RDH dump at runtime

    unsigned long long statsRdhCheckOk=0;   // number of RDH structs which have passed check ok
    unsigned long long statsRdhCheckErr=0;  // number of RDH structs which have not passed check    
    unsigned long long statsNumberOfPages=0; // number of pages read out
    unsigned long long statsNumberOfTimeframes=0; // number of timeframes read out
    
    
    AliceO2::Common::Timer timeframeClock;	// timeframe id should be increased at each clock cycle
    int currentTimeframe=0;	                // id of current timeframe
    bool usingSoftwareClock=false;              // if set, using internal software clock to generate timeframe id

    const unsigned int LHCBunches=3564;    // number of bunches in LHC
    const unsigned int LHCOrbitRate=11246; // LHC orbit rate, in Hz. 299792458 / 26659
    const uint32_t timeframePeriodOrbits=256;   // timeframe interval duration in number of LHC orbits
    
    uint32_t currentTimeframeHbOrbitBegin=0; // HbOrbit of beginning of timeframe 
    uint32_t firstTimeframeHbOrbitBegin=0; // HbOrbit of beginning of first timeframe
        
    size_t superPageSize=0; // usable size of a superpage
};


std::mutex readoutEquipmentRORCLock;
bool isDriverInitialized=false;


struct ReadoutEquipmentRORCException : virtual Exception {};

ReadoutEquipmentRORC::ReadoutEquipmentRORC(ConfigFile &cfg, std::string name) : ReadoutEquipment(cfg, name) {
   
  try {

    // get parameters from configuration
    // config keys are the same as the corresponding set functions in AliceO2::roc::Parameters
    
    std::string cardId=cfg.getValue<std::string>(name + ".cardId");
    
    int cfgChannelNumber=0;
    cfg.getOptionalValue<int>(name + ".channelNumber", cfgChannelNumber);

    int cfgGeneratorEnabled=0;
    cfg.getOptionalValue<int>(name + ".generatorEnabled", cfgGeneratorEnabled);
    
    int cfgGeneratorDataSize=8192;
    cfg.getOptionalValue<int>(name + ".generatorDataSize", cfgGeneratorDataSize);
    
    std::string cfgGeneratorLoopback="INTERNAL";
    cfg.getOptionalValue<std::string>(name + ".generatorLoopback", cfgGeneratorLoopback);

    std::string cfgGeneratorPattern="INCREMENTAL";
    cfg.getOptionalValue<std::string>(name + ".generatorPattern", cfgGeneratorPattern);
    
    int cfgGeneratorRandomSizeEnabled=0;
    cfg.getOptionalValue<int>(name + ".generatorRandomSizeEnabled", cfgGeneratorRandomSizeEnabled);
    
    std::string cfgLinkMask="0-31";
    cfg.getOptionalValue<std::string>(name + ".linkMask", cfgLinkMask);
    
    //std::string cfgReadoutMode="CONTINUOUS";
    //cfg.getOptionalValue<std::string>(name + ".readoutMode", cfgReadoutMode);
    
    std::string cfgResetLevel="INTERNAL";
    cfg.getOptionalValue<std::string>(name + ".resetLevel", cfgResetLevel);

    // extra configuration parameters    
    cfg.getOptionalValue<int>(name + ".rdhCheckEnabled", cfgRdhCheckEnabled);
    cfg.getOptionalValue<int>(name + ".rdhDumpEnabled", cfgRdhDumpEnabled);
        
/*    // get readout memory buffer parameters
    std::string sMemorySize=cfg.getValue<std::string>(name + ".memoryBufferSize");
    std::string sPageSize=cfg.getValue<std::string>(name + ".memoryPageSize");
    long long mMemorySize=ReadoutUtils::getNumberOfBytesFromString(sMemorySize.c_str());
    long long mPageSize=ReadoutUtils::getNumberOfBytesFromString(sPageSize.c_str());

    std::string cfgHugePageSize="1GB";
    cfg.getOptionalValue<std::string>(name + ".memoryHugePageSize",cfgHugePageSize);
*/
    // unique identifier based on card ID
    std::string uid="readout." + cardId + "." + std::to_string(cfgChannelNumber);
    //sleep((cfgChannelNumber+1)*2);  // trick to avoid all channels open at once - fail to acquire lock
    
    // define usable superpagesize
    superPageSize=mp->getPageSize()-pageSpaceReserved; // Keep space at beginning for DataBlock object
    superPageSize-=superPageSize % (32*1024); // Must be a multiple of 32Kb for ROC
    theLog.log("Using superpage size %ld",superPageSize);
    if (superPageSize==0) {
      BOOST_THROW_EXCEPTION(ReadoutEquipmentRORCException() << ErrorInfo::Message("Superpage must be at least 32kB"));
    }
  
    // make sure ROC driver is initialized once
    readoutEquipmentRORCLock.lock();       
    if (!isDriverInitialized) {
      AliceO2::roc::driver::initialize();
      isDriverInitialized=true;
    }
    readoutEquipmentRORCLock.unlock();
    
    // open and configure ROC
    theLog.log("Opening ROC %s:%d",cardId.c_str(),cfgChannelNumber);
    AliceO2::roc::Parameters params;
    params.setCardId(AliceO2::roc::Parameters::cardIdFromString(cardId));   
    params.setChannelNumber(cfgChannelNumber);

    // setDmaPageSize() : seems deprecated, let's not configure it

    // generator related parameters
    params.setGeneratorEnabled(cfgGeneratorEnabled);
    if (cfgGeneratorEnabled) {
      params.setGeneratorDataSize(cfgGeneratorDataSize);
      params.setGeneratorLoopback(AliceO2::roc::LoopbackMode::fromString(cfgGeneratorLoopback));
      params.setGeneratorPattern(AliceO2::roc::GeneratorPattern::fromString(cfgGeneratorPattern));
      params.setGeneratorRandomSizeEnabled(cfgGeneratorRandomSizeEnabled);
    }    

    // card readout mode : experimental, not needed
    // params.setReadoutMode(AliceO2::roc::ReadoutMode::fromString(cfgReadoutMode));    
  
    /*
    theLog.log("Loop DMA block %p:%lu", mp->getBaseBlockAddress(), mp->getBaseBlockSize());
    char *ptr=(char *)mp->getBaseBlockAddress();
    for (size_t i=0;i<mp->getBaseBlockSize();i++) {
      ptr[i]=0;
    }
    */

    // register the memory block for DMA
    void *baseAddress=(void *)mp->getBaseBlockAddress();
    size_t blockSize=mp->getBaseBlockSize();
    theLog.log("Register DMA block %p:%lu",baseAddress,blockSize);
    params.setBufferParameters(AliceO2::roc::buffer_parameters::Memory {
       baseAddress, blockSize
    });
       
    // clear locks if necessary
    params.setForcedUnlockEnabled(true);

    // define link mask
    // this is harmless for C-RORC
    params.setLinkMask(AliceO2::roc::Parameters::linkMaskFromString(cfgLinkMask));

    // open channel with above parameters
    channel = AliceO2::roc::ChannelFactory().getDmaChannel(params);  
    channel->resetChannel(AliceO2::roc::ResetLevel::fromString(cfgResetLevel));

    // retrieve card information
    std::string infoPciAddress=channel->getPciAddress().toString();
    int infoNumaNode=channel->getNumaNode();
    std::string infoSerialNumber="unknown";
    auto v_infoSerialNumber=channel->getSerial();
    if (v_infoSerialNumber) {
      infoSerialNumber=std::to_string(v_infoSerialNumber.get());
    }
    std::string infoFirmwareVersion=channel->getFirmwareInfo().value_or("unknown");
    std::string infoCardId=channel->getCardId().value_or("unknown");
    theLog.log("Equipment %s : PCI %s @ NUMA node %d, serial number %s, firmware version %s, card id %s", name.c_str(), infoPciAddress.c_str(), infoNumaNode, infoSerialNumber.c_str(),
    infoFirmwareVersion.c_str(), infoCardId.c_str());
    

    // todo: log parameters ?

    // start DMA    
    theLog.log("Starting DMA for ROC %s:%d",cardId.c_str(),cfgChannelNumber);
    channel->startDma();    
    
    // get FIFO depth (it should be fully empty when starting)
    RocFifoSize=channel->getTransferQueueAvailable();
    theLog.log("ROC input queue size = %d pages",RocFifoSize);
    if (RocFifoSize==0) {RocFifoSize=1;}

    // reset timeframe id
    currentTimeframe=0;
    if (!cfgRdhCheckEnabled) {
      usingSoftwareClock=true; // if RDH disabled, use internal clock for TF id
    }
    if (usingSoftwareClock) {
      // reset timeframe clock
      double timeframeRate=LHCOrbitRate*1.0/timeframePeriodOrbits; // timeframe rate, in Hz
      theLog.log("Timeframe IDs generated by software, %.2lf Hz",timeframeRate);
      timeframeClock.reset(1000000/timeframeRate);
    } else {
      theLog.log("Timeframe IDs generated from RDH trigger counters");
    }

  }
  catch (const std::exception& e) {
    std::cout << "Error: " << e.what() << '\n' << boost::diagnostic_information(e) << "\n";
    return;
  }
  isInitialized=true;
}



ReadoutEquipmentRORC::~ReadoutEquipmentRORC() {
  if (isInitialized) {
    channel->stopDma();
  }

  if (cfgRdhCheckEnabled) {
    theLog.log("Equipment %s : %llu timeframes, %llu pages, RDH checks %llu ok, %llu errors",name.c_str(),statsNumberOfTimeframes,statsNumberOfPages,statsRdhCheckOk,statsRdhCheckErr);  
  }
}


Thread::CallbackResult ReadoutEquipmentRORC::prepareBlocks(){
  if (!isInitialized) return  Thread::CallbackResult::Error;
  int isActive=0;
  
  // keep track of situations where the queue is completely empty
  // this means we have not filled it fast enough (except in first loop, where it's normal it is empty)
  if (isWaitingFirstLoop) {
    isWaitingFirstLoop=false;
  } else {
    int nFreeSlots=channel->getTransferQueueAvailable();
    if (nFreeSlots == RocFifoSize) {  
      equipmentStats[EquipmentStatsIndexes::nFifoUpEmpty].increment();
    }
    equipmentStats[EquipmentStatsIndexes::fifoOccupancyFreeBlocks].set(nFreeSlots);
  }
  
  // give free pages to the driver
  int nPushed=0;  // number of free pages pushed this iteration
  while (channel->getTransferQueueAvailable() != 0) {
    void *newPage=mp->getPage();
    if (newPage!=nullptr) {   
      // todo: check page is aligned as expected      
      AliceO2::roc::Superpage superpage;
      superpage.offset=(char *)newPage-(char *)mp->getBaseBlockAddress()+pageSpaceReserved;
      superpage.size=superPageSize;
      superpage.userData=newPage;
      channel->pushSuperpage(superpage);      
      isActive=1;
      nPushed++;
    } else {
      equipmentStats[EquipmentStatsIndexes::nMemoryLow].increment();
      isActive=0;
      break;
    }
  }
  equipmentStats[EquipmentStatsIndexes::nPushedUp].increment(nPushed);

  // check fifo occupancy ready queue size for stats
  equipmentStats[EquipmentStatsIndexes::fifoOccupancyReadyBlocks].set(channel->getReadyQueueSize());
  if (channel->getReadyQueueSize()==RocFifoSize) {
    equipmentStats[EquipmentStatsIndexes::nFifoReadyFull].increment();  
  }

  // if we have not put many pages (<25%) in ROC fifo, we can wait a bit
  if (nPushed<RocFifoSize/4) { 
    isActive=0;
  }


  // This global mutex was also used as a fix to allow reading out 2 CRORC at same time
  // otherwise machine reboots when ACPI is not OFF
  //readoutEquipmentRORCLock.lock();
  
  // this is to be called periodically for driver internal business
  channel->fillSuperpages();
  
  //readoutEquipmentRORCLock.unlock();


  // from time to time, we may monitor temperature
//      virtual boost::optional<float> getTemperature() = 0;


  if (!isActive) {
    return Thread::CallbackResult::Idle;
  }
  return Thread::CallbackResult::Ok;
}


DataBlockContainerReference ReadoutEquipmentRORC::getNextBlock() {

  DataBlockContainerReference nextBlock=nullptr;
  
  //channel->fillSuperpages();
    
  // check for completed page
  if ((channel->getReadyQueueSize()>0)) {
    auto superpage = channel->getSuperpage(); // this is the first superpage in FIFO ... let's check its state
    if (superpage.isFilled()) {
      std::shared_ptr<DataBlockContainer>d=nullptr;
      try {
        if (pageSpaceReserved>=sizeof(DataBlock)) {
          d=mp->getNewDataBlockContainer((void *)(superpage.userData));
        } else {
          // todo: allocate data block container elsewhere than beginning of page
          //d=mp->getNewDataBlockContainer(nullptr);        
          //d=mp->getNewDataBlockContainer((void *)(superpage.userData));
          //d=std::make_shared<DataBlockContainer>(nullptr);
        }
      }
      catch (...) {
        // todo: increment a stats counter?
        theLog.log("make_shared<DataBlock> failed");
      }
      if (d!=nullptr) {
        statsNumberOfPages++;
        
        d->getData()->header.dataSize=superpage.getReceived();
        d->getData()->header.linkId=0; // TODO

        channel->popSuperpage();
        nextBlock=d;
        
        // validate RDH structure, if configured to do so
        int linkId=-1;
        int hbOrbit=-1;
                
        // checks to do:
        // - HB clock consistent in all RDHs
        // - increasing counters

        if (cfgRdhCheckEnabled) {
          std::string errorDescription;
          size_t blockSize=d->getData()->header.dataSize;
          uint8_t *baseAddress=(uint8_t *)(d->getData()->data);
          for (size_t pageOffset=0;pageOffset<blockSize;) {
            RdhHandle h(baseAddress+pageOffset);
            
            if (linkId==-1) {
              linkId=h.getLinkId();
            } else {
              if (linkId!=h.getLinkId()) {
                printf("incosistent link ids: %d != %d\n",linkId,h.getLinkId());
              }
            }
            
            if (hbOrbit==-1) {
              hbOrbit=h.getHbOrbit();
              if ((statsNumberOfPages==1) || ((uint32_t)hbOrbit>=currentTimeframeHbOrbitBegin+timeframePeriodOrbits)) {
                if (statsNumberOfPages==1) {
                  firstTimeframeHbOrbitBegin=hbOrbit;
                }
                statsNumberOfTimeframes++;
                currentTimeframeHbOrbitBegin=hbOrbit-((hbOrbit-firstTimeframeHbOrbitBegin)%timeframePeriodOrbits); // keep it periodic and aligned to 1st timeframe
                int newTimeframe=1+(currentTimeframeHbOrbitBegin-firstTimeframeHbOrbitBegin)/timeframePeriodOrbits;
                if (newTimeframe!=currentTimeframe+1) {
                  printf("Non-contiguous timeframe IDs %d ... %d\n",currentTimeframe,newTimeframe);
                }
                currentTimeframe=newTimeframe;
                 //printf("Starting timeframe %d @ orbit %d (actual: %d)\n",currentTimeframe,(int)currentTimeframeHbOrbitBegin,(int)hbOrbit);
              } else {
                 //printf("HB orbit %d\n",hbOrbit);
              }
              
            }           
            
            //data format:
            // RDH v3 = https://docs.google.com/document/d/1otkSDYasqpVBDnxplBI7dWNxaZohctA-bvhyrzvtLoQ/edit?usp=sharing
            if (h.validateRdh(errorDescription)) {
              if (cfgRdhDumpEnabled) {
                for (int i=0;i<16;i++) {
                  printf("%08X ",(int)(((uint32_t*)baseAddress)[i]));
                }
                printf("\n");
                printf("Page 0x%p + %ld\n%s",(void *)baseAddress,pageOffset,errorDescription.c_str());
                h.dumpRdh();
                errorDescription.clear();
              }
              statsRdhCheckErr++;
            } else {
              statsRdhCheckOk++;

              if (cfgRdhDumpEnabled) {
                h.dumpRdh();
                for (int i=0;i<16;i++) {
                  printf("%08X ",(int)(((uint32_t*)baseAddress+pageOffset)[i]));
                }
                printf("\n");

              }
            }
            pageOffset+=h.getBlockLength();
          }
        }
        if (linkId>=0) {
          d->getData()->header.linkId=linkId;
        }

        if (usingSoftwareClock) {
	  if (timeframeClock.isTimeout()) {
	    currentTimeframe++;
            statsNumberOfTimeframes++;
	    timeframeClock.increment();
	  }
        }

        // set timeframe id
        d->getData()->header.id=currentTimeframe;        
      }
      else {
        // no data block container... what to do???
      }
    }
  }
  return nextBlock;
}


std::unique_ptr<ReadoutEquipment> getReadoutEquipmentRORC(ConfigFile &cfg, std::string cfgEntryPoint) {
  return std::make_unique<ReadoutEquipmentRORC>(cfg,cfgEntryPoint);
}
