#include "./SDUpdater_Class.hpp"

namespace SDUpdaterNS
{



  void SDUpdater::_error( const char **errMsgs, uint8_t msgCount, unsigned long waitdelay )
  {
    for( int i=0; i<msgCount; i++ ) {
      _error( String(errMsgs[i]), i<msgCount-1?0:waitdelay );
    }
  }


  void SDUpdater::_error( const String& errMsg, unsigned long waitdelay )
  {
    SDU_SERIAL.print("[ERROR] ");
    SDU_SERIAL.println( errMsg );
    if( cfg->onError ) cfg->onError( errMsg, waitdelay );
  }

  void SDUpdater::_message( const String& msg )
  {
    SDU_SERIAL.println( msg );
    if( cfg->onMessage ) cfg->onMessage( msg );
  }




  //***********************************************************************************************
  //                                B A C K T O F A C T O R Y                                     *
  //***********************************************************************************************
  // https://www.esp32.com/posting.php?mode=quote&f=2&p=19066&sid=5ba5f33d5fe650eb8a7c9f86eb5b61b8
  // Return to factory version.                                                                   *
  // This will set the otadata to boot from the factory image, ignoring previous OTA updates.     *
  //***********************************************************************************************
  void SDUpdater::loadFactory()
  {
    esp_partition_iterator_t  pi ;                   // Iterator for find
    const esp_partition_t*    factory ;              // Factory partition
    esp_err_t                 err ;

    pi = esp_partition_find ( ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL ) ;

    if ( pi == NULL ) {                              // Check result
      log_e( "Failed to find factory partition" ) ;
    } else {
      factory = esp_partition_get ( pi ) ;           // Get partition struct
      esp_partition_iterator_release ( pi ) ;        // Release the iterator
      err = esp_ota_set_boot_partition ( factory ) ; // Set partition for boot
      if ( err != ESP_OK ) {                         // Check error
        log_e( "Failed to set boot partition" ) ;
      } else {
        esp_restart() ;                              // Restart ESP
      }
    }
  }


  const esp_partition_t* SDUpdater::getFactoryPartition()
  {
    auto factorypi = esp_partition_find ( ESP_PARTITION_TYPE_APP,  ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL );
    if( factorypi != NULL ) {
      return esp_partition_get(factorypi);
    }
    return NULL;
  }



  esp_image_metadata_t SDUpdater::getSketchMeta( const esp_partition_t* source_partition )
  {
    esp_image_metadata_t data;
    if ( !source_partition ) {
      log_e("No source partition provided");
      return data;
    }
    const esp_partition_pos_t source_partition_pos  = {
      .offset = source_partition->address,
      .size = source_partition->size,
    };
    data.start_addr = source_partition_pos.offset;

    esp_app_desc_t app_desc;
    if( esp_ota_get_partition_description(source_partition, &app_desc) != ESP_OK ) {
      // nothing flashed here
      memset( data.image_digest, 0, sizeof(data.image_digest) );
      data.image_len = source_partition->size;//0;
      return data;
    }

    // only verify OTA partitions
    if( source_partition->type==ESP_PARTITION_TYPE_APP && (source_partition->subtype>=ESP_PARTITION_SUBTYPE_APP_OTA_MIN && source_partition->subtype<ESP_PARTITION_SUBTYPE_APP_OTA_MAX) ) {
      esp_err_t ret = esp_image_verify( ESP_IMAGE_VERIFY, &source_partition_pos, &data );
      if( ret != ESP_OK ) {
        log_e("Failed to verify image %s at addr %x", String( source_partition->label ), source_partition->address );
      } else {
        log_w("Successfully verified image %s at addr %x", String( source_partition->label[3] ), source_partition->address );
      }
    } else if( source_partition->type==ESP_PARTITION_TYPE_APP && source_partition->subtype==ESP_PARTITION_SUBTYPE_APP_FACTORY ) {
      // factory partition, compute the digest
      if( esp_partition_get_sha256(source_partition, data.image_digest) != ESP_OK ) {
        memset( data.image_digest, 0, sizeof(data.image_digest) );
        data.image_len = 0;
      }
    }
    return data;//.image_len;
  }



  bool SDUpdater::compareFsPartition(const esp_partition_t* src1, fs::File* src2, size_t length)
  {
    size_t lengthLeft = length;
    const size_t bufSize = SPI_FLASH_SEC_SIZE;
    std::unique_ptr<uint8_t[]> buf1(new uint8_t[bufSize]);
    std::unique_ptr<uint8_t[]> buf2(new uint8_t[bufSize]);
    uint32_t offset = 0;
    uint32_t progress = 0, progressOld = 1;
    size_t i;
    while( lengthLeft > 0) {
      size_t readBytes = (lengthLeft < bufSize) ? lengthLeft : bufSize;
      if (!ESP.flashRead(src1->address + offset, reinterpret_cast<uint32_t*>(buf1.get()), (readBytes + 3) & ~3)
      || !src2->read(                           reinterpret_cast<uint8_t*>(buf2.get()), (readBytes + 3) & ~3)
      ) {
          return false;
      }
      for (i = 0; i < readBytes; ++i) if (buf1[i] != buf2[i]) return false;
      lengthLeft -= readBytes;
      offset += readBytes;
      if( cfg->onProgress ) {
        progress = 100 * offset / length;
        if (progressOld != progress) {
          progressOld = progress;
          cfg->onProgress( (uint8_t)progress, 100 );
        }
      }
    }
    return true;
  }


  bool SDUpdater::copyFsPartition(fs::File* dst, const esp_partition_t* src, size_t length)
  {
    size_t lengthLeft = length;
    const size_t bufSize = SPI_FLASH_SEC_SIZE;
    std::unique_ptr<uint8_t[]> buf(new uint8_t[bufSize]);
    uint32_t offset = 0;
    uint32_t progress = 0, progressOld = 1;
    while( lengthLeft > 0) {
      size_t readBytes = (lengthLeft < bufSize) ? lengthLeft : bufSize;
      if (!ESP.flashRead(src->address + offset, reinterpret_cast<uint32_t*>(buf.get()), (readBytes + 3) & ~3)
      ) {
          return false;
      }
      if (dst) dst->write(buf.get(), (readBytes + 3) & ~3);
      lengthLeft -= readBytes;
      offset += readBytes;
      if( cfg->onProgress ) {
        progress = 100 * offset / length;
        if (progressOld != progress) {
          progressOld = progress;
          cfg->onProgress( (uint8_t)progress, 100 );
          vTaskDelay(10);
        }
      }
    }
    return true;
  }


  bool SDUpdater::saveSketchToFS( fs::FS &fs, const char* binfilename, bool skipIfExists )
  {
    // no rollback possible, start filesystem
    if( !_fsBegin( fs ) ) {
      const char *msg[] = {"No Filesystem mounted.","Can't check firmware."};
      _error( msg, 2 );
      return false;
    }

    if( skipIfExists ) {
      if( fs.exists( binfilename ) ) {
        log_d("File %s exists, skipping overwrite", binfilename );
        //_message( String("\nChecked ") + String(binfilename) );
        return false;
      }
    }
    if( cfg->onBefore) cfg->onBefore();
    if( cfg->onProgress ) cfg->onProgress( 0, 100 );
    const esp_partition_t *running = esp_ota_get_running_partition();
    size_t sksize = ESP.getSketchSize();
    bool ret = false;
    fs::File dst = fs.open(binfilename, FILE_WRITE );
    if( cfg->onProgress ) cfg->onProgress( 25, 100 );
    _message( String("Overwriting ") + String(binfilename) );

    if (copyFsPartition( &dst, running, sksize)) {
      if( cfg->onProgress ) cfg->onProgress( 75, 100 );
      _message( String("Done ") + String(binfilename) );
      vTaskDelay(1000);
      ret = true;
    } else {
      _error( "Copy failed" );
    }
    if( cfg->onProgress ) cfg->onProgress( 100, 100 );
    dst.close();
    if( cfg->onAfter) cfg->onAfter();

    return ret;
  }


  // rollback helper, save menu.bin meta info in NVS
  void SDUpdater::updateNVS()
  {
    const esp_partition_t* update_partition = esp_ota_get_next_update_partition( NULL );
    if (!update_partition) {
      log_d( "Canceling NVS Update as update partition is invalid" );
      return;
    }
    esp_image_metadata_t nusketchMeta = getSketchMeta( update_partition );
    uint32_t nuSize = nusketchMeta.image_len;
    SDU_SERIAL.printf( "Updating menu.bin NVS size/digest after update: %d\n", nuSize );
    Preferences preferences;
    preferences.begin( "sd-menu", false );
    preferences.putInt( "menusize", nuSize );
    preferences.putBytes( "digest", nusketchMeta.image_digest, 32 );
    preferences.end();
  }


  // perform the actual update from a given stream
  void SDUpdater::performUpdate( Stream &updateSource, size_t updateSize, String fileName )
  {
    assert(UpdateIface);
    UpdateIface->setBinName( fileName, &updateSource );

    _message( "LOADING " + fileName );
    log_d( "Binary size: %d bytes", updateSize );
    if( cfg->onProgress ) UpdateIface->onProgress( cfg->onProgress );
    if (UpdateIface->begin( updateSize )) {
      size_t written = UpdateIface->writeStream( updateSource, updateSize );
      if ( written == updateSize ) {
        SDU_SERIAL.println( "Written : " + String(written) + " successfully" );
      } else {
        SDU_SERIAL.println( "Written only : " + String(written) + "/" + String(updateSize) + ". Retry?" );
      }
      if ( UpdateIface->end() ) {
        SDU_SERIAL.println( "OTA done!" );
        if ( UpdateIface->isFinished() ) {
          if( strcmp( MenuBin, fileName.c_str() ) == 0 ) {
            // maintain NVS signature
            SDUpdater::updateNVS();
          }
          SDU_SERIAL.println( "Update successfully completed. Rebooting." );
        } else {
          SDU_SERIAL.println( "Update not finished? Something went wrong!" );
        }
      } else {
        SDU_SERIAL.println( "Update failed. Error #: " + String( UpdateIface->getError() ) );
      }
    } else {
      SDU_SERIAL.println( "Not enough space to begin OTA" );
    }
  }


  // forced rollback (doesn't check NVS digest)
  void SDUpdater::doRollBack( const String& message )
  {
    assert(UpdateIface);
    log_d("Wil check for rollback capability");
    if( !cfg->onMessage)   { log_d("No message reporting"); }
    //if( !cfg->onError )    log_d("No error reporting");
    if( !cfg->onProgress ) { log_d("No progress reporting"); }

    if( UpdateIface->canRollBack() ) {
      _message( message );
      for( uint8_t i=1; i<50; i++ ) {
        if( cfg->onProgress ) cfg->onProgress( i, 100 );
        vTaskDelay(10);
      }
      UpdateIface->rollBack();
      for( uint8_t i=50; i<=100; i++ ) {
        if( cfg->onProgress ) cfg->onProgress( i, 100 );
        vTaskDelay(10);
      }
      _message( "Rollback done, restarting" );
      ESP.restart();
    } else {
      const char *msg[] = {"Cannot rollback", "The other OTA", "partition doesn't", "seem to be", "populated or valid"};
      _error( msg, 5 );
    }
  }


  // if NVS has info about MENU_BIN flash size and digest, try rollback()
  void SDUpdater::tryRollback( String fileName )
  {
    Preferences preferences;
    preferences.begin( "sd-menu" );
    uint32_t menuSize = preferences.getInt( "menusize", 0 );
    uint8_t image_digest[32];
    preferences.getBytes( "digest", image_digest, 32 );
    preferences.end();
    SDU_SERIAL.println( "Trying rollback" );

    if( menuSize == 0 ) {
      log_d( "Failed to get expected menu size from NVS ram, can't check if rollback is worth a try..." );
      return;
    }

    const esp_partition_t* update_partition = esp_ota_get_next_update_partition( NULL );
    if (!update_partition) {
      log_d( "Cancelling rollback as update partition is invalid" );
      return;
    }
    esp_image_metadata_t sketchMeta = getSketchMeta( update_partition );
    uint32_t nuSize = sketchMeta.image_len;

    if( nuSize != menuSize ) {
      log_d( "Cancelling rollback as flash sizes differ, update / current : %d / %d",  nuSize, menuSize );
      return;
    }

    SDU_SERIAL.println( "Sizes match! Checking digest..." );
    bool match = true;
    for( uint8_t i=0; i<32; i++ ) {
      if( image_digest[i]!=sketchMeta.image_digest[i] ) {
        SDU_SERIAL.println( "NO match for NVS digest :-(" );
        match = false;
        break;
      }
    }
    if( match ) {
      doRollBack( "HOT-LOADING " + fileName );
    }
  }


  // do perform update
  void SDUpdater::updateFromStream( Stream &stream, size_t updateSize, const String& fileName )
  {
    if ( updateSize > 0 ) {
      SDU_SERIAL.println( "Try to start update" );
      disableCore0WDT(); // disable WDT it as suggested by twitter.com/@lovyan03
      performUpdate( stream, updateSize, fileName );
      enableCore0WDT();
    } else {
      _error( "Stream is empty" );
    }
  }


  void SDUpdater::updateFromFS( fs::FS &fs, const String& fileName )
  {
    cfg->setFS( &fs );
    updateFromFS( fileName );
  }


  void SDUpdater::checkSDUpdaterHeadless( fs::FS &fs, String fileName, unsigned long waitdelay )
  {
    cfg->setFS( &fs );
    checkSDUpdaterHeadless( fileName, waitdelay );
  }


  void SDUpdater::checkSDUpdaterUI( fs::FS &fs, String fileName, unsigned long waitdelay )
  {
    cfg->setFS( &fs );
    checkSDUpdaterUI( fileName, waitdelay );
  }


  void SDUpdater::updateFromFS( const String& fileName )
  {
    if( cfg->fs == nullptr ) {
      const char *msg[] = {"No valid filesystem", "selected!"};
      _error( msg, 2 );
      return;
    }
    SDU_SERIAL.printf( "[" SD_PLATFORM_NAME "-SD-Updater] SD Updater version: %s\n", (char*)M5_SD_UPDATER_VERSION );
    #ifdef M5_LIB_VERSION
      SDU_SERIAL.printf( "[" SD_PLATFORM_NAME "-SD-Updater] M5Stack Core version: %s\n", (char*)M5_LIB_VERSION );
    #endif
    SDU_SERIAL.printf( "[" SD_PLATFORM_NAME "-SD-Updater] Application was Compiled on %s %s\n", __DATE__, __TIME__ );
    SDU_SERIAL.printf( "[" SD_PLATFORM_NAME "-SD-Updater] Will attempt to load binary %s \n", fileName.c_str() );

    // try rollback first, it's faster!
    if( strcmp( MenuBin, fileName.c_str() ) == 0 ) {
      if( cfg->use_rollback ) {
        tryRollback( fileName );
        log_e("Rollback failed, will try from filesystem");
      } else {
        log_d("Skipping rollback per config");
      }
    }
    // no rollback possible, start filesystem
    if( !_fsBegin() ) {
      const char* msg[] = {"No filesystem mounted.", "Can't load firmware."};
      _error( msg, 2 );
      return;
    }

    File updateBin = cfg->fs->open( fileName );
    if ( updateBin ) {

      if( updateBin.isDirectory() ) {
        updateBin.close();
        _error( fileName + " is a directory" );
        return;
      }

      size_t updateSize = updateBin.size();

      updateFromStream( updateBin, updateSize, fileName );

      updateBin.close();

    } else {
      const char* msg[] = {"Could not reach", fileName.c_str(), "Can't load firmware."};
      _error( msg, 3 );
    }
  }


  // check given FS for valid menu.bin and perform update if available
  void SDUpdater::checkSDUpdaterHeadless( String fileName, unsigned long waitdelay )
  {
    if( waitdelay == 0 ) {
      waitdelay = 100; // at least give some time for the serial buffer to fill
    }
    SDU_SERIAL.printf("SDUpdater: you have %d milliseconds to send 'update', 'rollback', 'skip' or 'save' command\n", (int)waitdelay);

    if( cfg->onWaitForAction ) {
      int ret = cfg->onWaitForAction( nullptr, nullptr, nullptr, waitdelay );
      if ( ret == ConfigManager::SDU_BTNA_MENU ) {
        SDU_SERIAL.printf( SDU_LOAD_TPL, fileName.c_str() );
        updateFromFS( fileName );
        ESP.restart();
      }
      if( cfg->binFileName != nullptr ) {
        log_d("Checking if %s needs saving", cfg->binFileName );
        saveSketchToFS( *cfg->fs,  cfg->binFileName, ret != ConfigManager::SDU_BTNC_SAVE );
      }
    } else {
      _error( "Missing onWaitForAction!" );
    }

    SDU_SERIAL.println("Delay expired, no SD-Update will occur");
  }


  void SDUpdater::checkSDUpdaterUI( String fileName, unsigned long waitdelay )
  {
    if( cfg->fs == nullptr ) {
      const char* msg[] = {"No valid filesystem", "selected!", "Cannot load", fileName.c_str()};
      _error( msg, 4 );
      return;
    }
    bool draw = SDUHasTouch;
    bool isRollBack = true;
    if( fileName != "" ) {
      isRollBack = false;
    }

    if( !draw ) { // default touch button support
      if( waitdelay <= 100 ) {
        // no UI draw, but still attempt to detect "button is pressed on boot"
        // round up to 100ms for button debounce
        waitdelay = 100;
      } else {
        // only force draw if waitdelay > 100
        draw = true;
      }
    }

    if( draw ) { // bring up the UI
      if( cfg->onBefore) cfg->onBefore();
      if( cfg->onSplashPage) cfg->onSplashPage( BTN_HINT_MSG );
    }

    if( cfg->onWaitForAction ) {
      [[maybe_unused]] unsigned long startwait = millis();
      int ret = cfg->onWaitForAction( isRollBack ? (char*)cfg->labelRollback : (char*)cfg->labelMenu,  (char*)cfg->labelSkip, (char*)cfg->labelSave, waitdelay );
      [[maybe_unused]] unsigned long actualwaitdelay = millis()-startwait;

      log_v("Action '%d' was triggered after %d ms (waidelay=%d)", ret, actualwaitdelay, waitdelay );

      if ( ret == ConfigManager::SDU_BTNA_MENU ) {
        if( isRollBack == false ) {
          SDU_SERIAL.printf( SDU_LOAD_TPL, fileName.c_str() );
          updateFromFS( fileName );
          ESP.restart();
        } else {
          SDU_SERIAL.println( SDU_ROLLBACK_MSG );
          doRollBack( SDU_ROLLBACK_MSG );
        }
      }
      if( cfg->binFileName != nullptr ) {
        log_d("Checking if %s needs saving", cfg->binFileName );
        saveSketchToFS( *cfg->fs,  cfg->binFileName, ret != ConfigManager::SDU_BTNC_SAVE );
      }
    } else {
      _error( "Missing onWaitForAction!" );
    }

    if( draw ) {
      // reset text styles to avoid messing with the overlayed application
      if( cfg->onAfter ) cfg->onAfter();
    }
  }


}; // end namespace
