/*!
 * \file COutput.cpp
 * \brief Main subroutines for output solver information
 * \author F. Palacios, T. Economon
 * \version 7.5.1 "Blackbird"
 *
 * SU2 Project Website: https://su2code.github.io
 *
 * The SU2 Project is maintained by the SU2 Foundation
 * (http://su2foundation.org)
 *
 * Copyright 2012-2023, SU2 Contributors (cf. AUTHORS.md)
 *
 * SU2 is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * SU2 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with SU2. If not, see <http://www.gnu.org/licenses/>.
 */

#include "../../../Common/include/geometry/CGeometry.hpp"
#include "../../include/solvers/CSolver.hpp"

#include "../../include/output/COutput.hpp"
#include "../../include/output/filewriter/CFVMDataSorter.hpp"
#include "../../include/output/filewriter/CFEMDataSorter.hpp"
#include "../../include/output/filewriter/CCGNSFileWriter.hpp"
#include "../../include/output/filewriter/CSurfaceFVMDataSorter.hpp"
#include "../../include/output/filewriter/CSurfaceFEMDataSorter.hpp"
#include "../../include/output/filewriter/CParaviewFileWriter.hpp"
#include "../../include/output/filewriter/CSTLFileWriter.hpp"
#include "../../include/output/filewriter/CParaviewBinaryFileWriter.hpp"
#include "../../include/output/filewriter/CParaviewXMLFileWriter.hpp"
#include "../../include/output/filewriter/CParaviewVTMFileWriter.hpp"
#include "../../include/output/filewriter/CTecplotFileWriter.hpp"
#include "../../include/output/filewriter/CTecplotBinaryFileWriter.hpp"
#include "../../include/output/filewriter/CCSVFileWriter.hpp"
#include "../../include/output/filewriter/CSU2FileWriter.hpp"
#include "../../include/output/filewriter/CSU2BinaryFileWriter.hpp"
#include "../../include/output/filewriter/CSU2MeshFileWriter.hpp"

COutput::COutput(const CConfig *config, unsigned short ndim, bool fem_output):
  rank(SU2_MPI::GetRank()),
  size(SU2_MPI::GetSize()),
  nDim(ndim),
  multiZone(config->GetMultizone_Problem()),
  gridMovement(config->GetDynamic_Grid()),
  femOutput(fem_output),
  si_units(config->GetSystemMeasurements() == SI),
  us_units(config->GetSystemMeasurements() == US) {

  cauchyTimeConverged = false;

  convergenceTable = new PrintingToolbox::CTablePrinter(&std::cout);
  multiZoneHeaderTable = new PrintingToolbox::CTablePrinter(&std::cout);
  fileWritingTable = new PrintingToolbox::CTablePrinter(&std::cout);
  historyFileTable = new PrintingToolbox::CTablePrinter(&histFile, "");

  /*--- Set default filenames ---*/

  surfaceFilename = "surface";
  volumeFilename  = "volume";
  restartFilename = "restart";

  /*--- Retrieve the history filename ---*/

  historyFilename = config->GetConv_FileName();

  /*--- Add the correct file extension depending on the file format ---*/

  string hist_ext = ".csv";
  if (config->GetTabular_FileFormat() == TAB_OUTPUT::TAB_TECPLOT) hist_ext = ".dat";

  /*--- Append the zone ID ---*/

  historyFilename = config->GetMultizone_HistoryFileName(historyFilename, config->GetiZone(), hist_ext);

  /*--- Append the restart iteration ---*/

  if (config->GetTime_Domain() && config->GetRestart()) {
    historyFilename = config->GetUnsteady_FileName(historyFilename, config->GetRestart_Iter(), hist_ext);
  }

  historySep = ",";

  /*--- Initialize residual ---*/

  rhoResNew = EPS;
  rhoResOld = EPS;

  nRequestedHistoryFields = config->GetnHistoryOutput();
  for (unsigned short iField = 0; iField < nRequestedHistoryFields; iField++){
    requestedHistoryFields.push_back(config->GetHistoryOutput_Field(iField));
  }

  nRequestedScreenFields = config->GetnScreenOutput();
  for (unsigned short iField = 0; iField < nRequestedScreenFields; iField++){
    requestedScreenFields.push_back(config->GetScreenOutput_Field(iField));
  }

  nRequestedVolumeFields = config->GetnVolumeOutput();
  for (unsigned short iField = 0; iField < nRequestedVolumeFields; iField++){
    requestedVolumeFields.push_back(config->GetVolumeOutput_Field(iField));
  }

  /*--- Default is to write history to file and screen --- */

  noWriting = false;

  /*--- Initialize convergence monitoring structure ---*/

  nCauchy_Elems = config->GetCauchy_Elems();
  cauchyEps = config->GetCauchy_Eps();
  minLogResidual = config->GetMinLogResidual();

  for (unsigned short iField = 0; iField < config->GetnConv_Field(); iField++){
    convFields.emplace_back(config->GetConv_Field(iField));
  }

  newFunc = vector<su2double>(convFields.size());
  oldFunc = vector<su2double>(convFields.size());
  cauchySerie = vector<vector<su2double>>(convFields.size(), vector<su2double>(nCauchy_Elems, 0.0));
  cauchyValue = 0.0;
  convergence = false;

  /*--- Initialize time convergence monitoring structure ---*/

  nWndCauchy_Elems = config->GetWnd_Cauchy_Elems();
  wndCauchyEps     = config->GetWnd_Cauchy_Eps();

  wndConvFields.reserve(config->GetnWndConv_Field());
  for (unsigned short iField = 0; iField < config->GetnWndConv_Field(); iField++){
    wndConvFields.emplace_back(config->GetWndConv_Field(iField));
  }

  WndOld_Func = vector<su2double>(wndConvFields.size());
  WndNew_Func = vector<su2double>(wndConvFields.size());
  WndCauchy_Serie = vector<vector<su2double>>(wndConvFields.size(), vector<su2double>(nWndCauchy_Elems, 0.0));
  WndCauchy_Value = 0.0;
  TimeConvergence = false;

  /*--- Check that the number of cauchy elems is not too large ---*/

  if (nCauchy_Elems > 1000){
    SU2_MPI::Error("Number of Cauchy Elems must be smaller than 1000", CURRENT_FUNCTION);
  }
  if (nWndCauchy_Elems > 1000){
    SU2_MPI::Error("Number of Time Cauchy Elems must be smaller than 1000", CURRENT_FUNCTION);
  }

  /*--- Initialize all convergence flags to false. ---*/

  convergence        = false;

  buildFieldIndexCache = false;

  curInnerIter = 0;
  curOuterIter = 0;
  curTimeIter  = 0;

  volumeDataSorter = nullptr;
  surfaceDataSorter = nullptr;

  headerNeeded = false;

}

COutput::~COutput(void) {

  delete convergenceTable;
  delete multiZoneHeaderTable;
  delete fileWritingTable;
  delete historyFileTable;
  delete volumeDataSorter;
  delete surfaceDataSorter;

}

void COutput::SetHistoryOutput(CGeometry *geometry,
                                  CSolver **solver_container,
                                  CConfig *config,
                                  unsigned long TimeIter,
                                  unsigned long OuterIter,
                                  unsigned long InnerIter) {

  curTimeIter  = TimeIter;
  curAbsTimeIter = TimeIter - config->GetRestart_Iter();
  curOuterIter = OuterIter;
  curInnerIter = InnerIter;

  /*--- Retrieve residual and extra data -----------------------------------------------------------------*/

  LoadCommonHistoryData(config);

  LoadHistoryData(config, geometry, solver_container);

  ConvergenceMonitoring(config, curInnerIter);

  PostprocessHistoryData(config);

  MonitorTimeConvergence(config, curTimeIter);

  OutputScreenAndHistory(config);

}

void COutput::SetHistoryOutput(CGeometry *geometry,
                                CSolver **solver_container,
                                CConfig *config) {

  /*--- Retrieve residual and extra data -----------------------------------------------------------------*/

  LoadCommonHistoryData(config);

  LoadHistoryData(config, geometry, solver_container);

  ConvergenceMonitoring(config, curInnerIter);

  PostprocessHistoryData(config);

}

void COutput::SetMultizoneHistoryOutput(COutput **output, CConfig **config, CConfig *driver_config, unsigned long TimeIter, unsigned long OuterIter){

  curTimeIter  = TimeIter;
  curAbsTimeIter = TimeIter - driver_config->GetRestart_Iter();
  curOuterIter = OuterIter;

  /*--- Retrieve residual and extra data -----------------------------------------------------------------*/

  LoadCommonHistoryData(driver_config);

  LoadMultizoneHistoryData(output, config);

  ConvergenceMonitoring(driver_config, curOuterIter);

  PostprocessHistoryData(driver_config);

  MonitorTimeConvergence(driver_config, curTimeIter);

  OutputScreenAndHistory(driver_config);

}

void COutput::OutputScreenAndHistory(CConfig *config) {

  if (rank == MASTER_NODE && !noWriting) {

    if (WriteHistoryFileOutput(config)) SetHistoryFileOutput(config);

    if (WriteScreenHeader(config)) SetScreenHeader(config);

    if (WriteScreenOutput(config)) SetScreenOutput(config);

  }
}

void COutput::SetupCustomHistoryOutput(const std::string& expression, CustomHistoryOutput& output) const {

  std::vector<std::string> symbols;
  output.expression = mel::Parse<passivedouble>(expression, symbols);

  output.symbolValues.reserve(symbols.size());
  for (const auto& symbol : symbols) {
    const auto* ptr = GetPtrToHistoryOutput(symbol);
    if (ptr == nullptr) {
      SU2_MPI::Error(std::string("Invalid history output (") + symbol + std::string(") used in expression:\n") +
                     expression, CURRENT_FUNCTION);
    }
    output.symbolValues.push_back(ptr);
  }
  output.ready = true;
}

void COutput::SetCustomAndComboObjectives(int idxSol, const CConfig *config, CSolver **solver) {

  if (config->GetKind_ObjFunc() == CUSTOM_OBJFUNC && !config->GetCustomObjFunc().empty()) {
    if (!customObjFunc.ready) {
      SetupCustomHistoryOutput(config->GetCustomObjFunc(), customObjFunc);
    }
    solver[idxSol]->SetTotal_Custom_ObjFunc(customObjFunc.Eval());
  }
  solver[idxSol]->Evaluate_ObjFunc(config, solver);
  SetHistoryOutputValue("COMBO", solver[idxSol]->GetTotal_ComboObj());
}

void COutput::AllocateDataSorters(CConfig *config, CGeometry *geometry){

  /*---- Construct a data sorter object to partition and distribute
   *  the local data into linear chunks across the processors ---*/

  if (femOutput){

    if (volumeDataSorter == nullptr)
      volumeDataSorter = new CFEMDataSorter(config, geometry, volumeFieldNames);

    if (surfaceDataSorter == nullptr)
      surfaceDataSorter = new CSurfaceFEMDataSorter(config, geometry,
                                                  dynamic_cast<CFEMDataSorter*>(volumeDataSorter));

  }  else {

    if (volumeDataSorter == nullptr)
      volumeDataSorter = new CFVMDataSorter(config, geometry, volumeFieldNames);

    if (surfaceDataSorter == nullptr)
      surfaceDataSorter = new CSurfaceFVMDataSorter(config, geometry,
                                                  dynamic_cast<CFVMDataSorter*>(volumeDataSorter));

  }

}

void COutput::LoadData(CGeometry *geometry, CConfig *config, CSolver** solver_container){

  /*--- Check if the data sorters are allocated, if not, allocate them. --- */

  AllocateDataSorters(config, geometry);

  /*--- Loop over all points and store the requested volume output data into the data sorter objects ---*/

  LoadDataIntoSorter(config, geometry, solver_container);

  /*--- Partition and sort the volume output data -- */

  volumeDataSorter->SortOutputData();

}

void COutput::WriteToFile(CConfig *config, CGeometry *geometry, OUTPUT_TYPE format, string fileName){

  CFileWriter *fileWriter = nullptr;

  /*--- if it is still present, strip the extension (suffix) from the filename ---*/
  unsigned short lastindex = fileName.find_last_of(".");
  fileName = fileName.substr(0, lastindex);

  string filename_iter, extension;

  /*--- Write files depending on the format --- */

  switch (format) {

    case OUTPUT_TYPE::SURFACE_CSV:

      extension = CSU2FileWriter::fileExt;

      if (fileName.empty())
        fileName = config->GetFilename(surfaceFilename, "", curTimeIter);

      if (!config->GetWrt_Surface_Overwrite())
        filename_iter = config->GetFilename_Iter(fileName,curInnerIter, curOuterIter);

      surfaceDataSorter->SortConnectivity(config, geometry);
      surfaceDataSorter->SortOutputData();

      if (rank == MASTER_NODE) {
        (*fileWritingTable) << "CSV file" << fileName + extension;

        if (!config->GetWrt_Surface_Overwrite())
          (*fileWritingTable) << "CSV file + iter" << filename_iter + extension;
      }

      fileWriter = new CSU2FileWriter(surfaceDataSorter);

      break;

    case OUTPUT_TYPE::RESTART_ASCII: case OUTPUT_TYPE::CSV:

      extension = CSU2FileWriter::fileExt;

      if (fileName.empty())
        fileName = config->GetFilename(restartFilename, "", curTimeIter);

      if (!config->GetWrt_Restart_Overwrite())
        filename_iter = config->GetFilename_Iter(fileName,curInnerIter, curOuterIter);


      if (rank == MASTER_NODE) {
        (*fileWritingTable) << "SU2 ASCII restart" << fileName + extension;

        if (!config->GetWrt_Restart_Overwrite())
          (*fileWritingTable) << "SU2 ASCII restart + iter" << filename_iter + extension;
      }

      fileWriter = new CSU2FileWriter(volumeDataSorter);

      break;

    case OUTPUT_TYPE::RESTART_BINARY:

      extension = CSU2BinaryFileWriter::fileExt;

      if (fileName.empty())
        fileName = config->GetFilename(restartFilename, "", curTimeIter);

      if (!config->GetWrt_Restart_Overwrite())
        filename_iter = config->GetFilename_Iter(fileName,curInnerIter, curOuterIter);


      if (rank == MASTER_NODE) {
        (*fileWritingTable) << "SU2 binary restart" << fileName + extension;

        if (!config->GetWrt_Restart_Overwrite())
          (*fileWritingTable) << "SU2 binary restart + iter" << filename_iter + extension;

      }

      fileWriter = new CSU2BinaryFileWriter(volumeDataSorter);


      break;

    case OUTPUT_TYPE::MESH:

      extension = CSU2MeshFileWriter::fileExt;

      if (fileName.empty())
        fileName = volumeFilename;

      if (!config->GetWrt_Volume_Overwrite())
        filename_iter = config->GetFilename_Iter(fileName,curInnerIter, curOuterIter);

      /*--- Load and sort the output data and connectivity. ---*/

      volumeDataSorter->SortConnectivity(config, geometry, true);

      /*--- Set the mesh ASCII format ---*/
      if (rank == MASTER_NODE) {
        (*fileWritingTable) << "SU2 mesh" << fileName + extension;

        if (!config->GetWrt_Volume_Overwrite())
          (*fileWritingTable) << "SU2 mesh + iter" << filename_iter + extension;
      }

      fileWriter = new CSU2MeshFileWriter(volumeDataSorter,
                                          config->GetiZone(), config->GetnZone());


      break;

    case OUTPUT_TYPE::TECPLOT_BINARY:

      extension = CTecplotBinaryFileWriter::fileExt;

      if (fileName.empty())
        fileName = config->GetFilename(volumeFilename, "", curTimeIter);

      if (!config->GetWrt_Volume_Overwrite())
        filename_iter = config->GetFilename_Iter(fileName,curInnerIter, curOuterIter);

      /*--- Load and sort the output data and connectivity. ---*/

      volumeDataSorter->SortConnectivity(config, geometry, false);

      /*--- Write tecplot binary ---*/
      if (rank == MASTER_NODE) {
        (*fileWritingTable) << "Tecplot binary" << fileName + extension;

        if (!config->GetWrt_Volume_Overwrite())
          (*fileWritingTable) << "Tecplot binary + iter" << filename_iter + extension;
      }

      fileWriter = new CTecplotBinaryFileWriter(volumeDataSorter,
                                                curTimeIter, GetHistoryFieldValue("TIME_STEP"));

      break;

    case OUTPUT_TYPE::TECPLOT_ASCII:

      extension = CTecplotFileWriter::fileExt;

      if (fileName.empty())
        fileName = config->GetFilename(volumeFilename, "", curTimeIter);

      if (!config->GetWrt_Volume_Overwrite())
        filename_iter = config->GetFilename_Iter(fileName,curInnerIter, curOuterIter);

      /*--- Load and sort the output data and connectivity. ---*/

      volumeDataSorter->SortConnectivity(config, geometry, true);

      /*--- Write tecplot ascii ---*/
      if (rank == MASTER_NODE) {
        (*fileWritingTable) << "Tecplot ASCII" << fileName + extension;

        if (!config->GetWrt_Volume_Overwrite())
          (*fileWritingTable) << "Tecplot ASCII + iter" << filename_iter + extension;
      }

      fileWriter = new CTecplotFileWriter(volumeDataSorter,
                                          curTimeIter, GetHistoryFieldValue("TIME_STEP"));

      break;

    case OUTPUT_TYPE::PARAVIEW_XML:

      extension = CParaviewXMLFileWriter::fileExt;

      if (fileName.empty())
        fileName = config->GetFilename(volumeFilename, "", curTimeIter);

      if (!config->GetWrt_Volume_Overwrite())
        filename_iter = config->GetFilename_Iter(fileName,curInnerIter, curOuterIter);

      /*--- Load and sort the output data and connectivity. ---*/

      volumeDataSorter->SortConnectivity(config, geometry, true);

      /*--- Write paraview binary ---*/
      if (rank == MASTER_NODE) {
        (*fileWritingTable) << "Paraview" << fileName + extension;

        if (!config->GetWrt_Volume_Overwrite())
          (*fileWritingTable) << "Paraview + iter" << filename_iter + extension;
      }

      fileWriter = new CParaviewXMLFileWriter(volumeDataSorter);

      break;

    case OUTPUT_TYPE::PARAVIEW_LEGACY_BINARY:

      extension = CParaviewBinaryFileWriter::fileExt;

      if (fileName.empty())
        fileName = config->GetFilename(volumeFilename, "", curTimeIter);

      if (!config->GetWrt_Volume_Overwrite())
        filename_iter = config->GetFilename_Iter(fileName,curInnerIter, curOuterIter);

      /*--- Load and sort the output data and connectivity. ---*/

      volumeDataSorter->SortConnectivity(config, geometry, true);

      /*--- Write paraview binary ---*/
      if (rank == MASTER_NODE) {
        (*fileWritingTable) << "Paraview binary (legacy)" << fileName + extension;

        if (!config->GetWrt_Volume_Overwrite())
          (*fileWritingTable) << "Paraview binary + iter" << filename_iter + extension;
      }

      fileWriter = new CParaviewBinaryFileWriter(volumeDataSorter);

      break;

    case OUTPUT_TYPE::PARAVIEW_MULTIBLOCK:
      {

        extension = CParaviewVTMFileWriter::fileExt;

        if (fileName.empty())
          fileName = config->GetUnsteady_FileName(volumeFilename, curTimeIter, "");

        if (!config->GetWrt_Volume_Overwrite())
          filename_iter = config->GetFilename_Iter(fileName,curInnerIter, curOuterIter);

        /*--- Sort volume connectivity ---*/

        volumeDataSorter->SortConnectivity(config, geometry, true);

        if (rank == MASTER_NODE) {
          (*fileWritingTable) << "Paraview Multiblock" << fileName + extension;

          if (!config->GetWrt_Volume_Overwrite())
            (*fileWritingTable) << "Paraview Multiblock + iter" << filename_iter + extension;
        }

        /*--- Allocate the vtm file writer (using fileName as the folder name) ---*/

        fileWriter = new CParaviewVTMFileWriter(GetHistoryFieldValue("CUR_TIME"),
                                                config->GetiZone(), config->GetnZone());

        /*--- We cast the pointer to its true type, to avoid virtual functions ---*/

        CParaviewVTMFileWriter* vtmWriter = dynamic_cast<CParaviewVTMFileWriter*>(fileWriter);

        /*--- then we write the data into the folder---*/
        vtmWriter->WriteFolderData(fileName, config, multiZoneHeaderString, volumeDataSorter,surfaceDataSorter, geometry);

        /*--- and we write the data into the folder with the iteration number ---*/
        if (!config->GetWrt_Volume_Overwrite())
          vtmWriter->WriteFolderData(filename_iter, config, multiZoneHeaderString, volumeDataSorter,surfaceDataSorter, geometry);

      }

      break;

    case OUTPUT_TYPE::PARAVIEW_ASCII:

      extension = CParaviewFileWriter::fileExt;

      if (fileName.empty())
        fileName = config->GetFilename(volumeFilename, "", curTimeIter);

      if (!config->GetWrt_Volume_Overwrite())
        filename_iter = config->GetFilename_Iter(fileName,curInnerIter, curOuterIter);


      /*--- Load and sort the output data and connectivity. ---*/

      volumeDataSorter->SortConnectivity(config, geometry, true);

      /*--- Write paraview ascii ---*/
      if (rank == MASTER_NODE) {
        (*fileWritingTable) << "Paraview ASCII" << fileName + extension;

        if (!config->GetWrt_Volume_Overwrite())
          (*fileWritingTable) << "Paraview ASCII + iter" << filename_iter + extension;
      }

      fileWriter = new CParaviewFileWriter(volumeDataSorter);

      break;

    case OUTPUT_TYPE::SURFACE_PARAVIEW_ASCII:

      extension = CParaviewFileWriter::fileExt;

      if (fileName.empty())
        fileName = config->GetFilename(surfaceFilename, "", curTimeIter);

      if (!config->GetWrt_Surface_Overwrite())
        filename_iter = config->GetFilename_Iter(fileName,curInnerIter, curOuterIter);


      /*--- Load and sort the output data and connectivity. ---*/

      surfaceDataSorter->SortConnectivity(config, geometry);
      surfaceDataSorter->SortOutputData();

      /*--- Write surface paraview ascii ---*/
      if (rank == MASTER_NODE) {
        (*fileWritingTable) << "Paraview ASCII surface" << fileName + extension;

        if (!config->GetWrt_Surface_Overwrite())
          (*fileWritingTable) << "Paraview ASCII + iter" << filename_iter + extension;
      }

      fileWriter = new CParaviewFileWriter(surfaceDataSorter);

      break;

    case OUTPUT_TYPE::SURFACE_PARAVIEW_LEGACY_BINARY:

        extension = CParaviewBinaryFileWriter::fileExt;

      if (fileName.empty())
        fileName = config->GetFilename(surfaceFilename, "", curTimeIter);

      if (!config->GetWrt_Surface_Overwrite())
        filename_iter = config->GetFilename_Iter(fileName,curInnerIter, curOuterIter);


      /*--- Load and sort the output data and connectivity. ---*/

      surfaceDataSorter->SortConnectivity(config, geometry);
      surfaceDataSorter->SortOutputData();

      /*--- Write surface paraview binary ---*/
      if (rank == MASTER_NODE) {
        (*fileWritingTable) << "Paraview binary surface (legacy)" << fileName + extension;

        if (!config->GetWrt_Surface_Overwrite())
          (*fileWritingTable) << "Paraview binary surface + iter" << filename_iter + extension;
      }

      fileWriter = new CParaviewBinaryFileWriter(surfaceDataSorter);

      break;

    case OUTPUT_TYPE::SURFACE_PARAVIEW_XML:

      extension = CParaviewXMLFileWriter::fileExt;

      if (fileName.empty())
        fileName = config->GetFilename(surfaceFilename, "", curTimeIter);

      if (!config->GetWrt_Surface_Overwrite())
        filename_iter = config->GetFilename_Iter(fileName,curInnerIter, curOuterIter);


      /*--- Load and sort the output data and connectivity. ---*/

      surfaceDataSorter->SortConnectivity(config, geometry);
      surfaceDataSorter->SortOutputData();

      /*--- Write paraview binary ---*/
      if (rank == MASTER_NODE) {
        (*fileWritingTable) << "Paraview surface" << fileName + extension;

        if (!config->GetWrt_Surface_Overwrite())
          (*fileWritingTable) << "Paraview surface + iter" << filename_iter + extension;
      }

      fileWriter = new CParaviewXMLFileWriter(surfaceDataSorter);

      break;

    case OUTPUT_TYPE::SURFACE_TECPLOT_ASCII:

        extension = CTecplotFileWriter::fileExt;

      if (fileName.empty())
        fileName = config->GetFilename(surfaceFilename, "", curTimeIter);

      if (!config->GetWrt_Surface_Overwrite())
        filename_iter = config->GetFilename_Iter(fileName,curInnerIter, curOuterIter);


      /*--- Load and sort the output data and connectivity. ---*/

      surfaceDataSorter->SortConnectivity(config, geometry);
      surfaceDataSorter->SortOutputData();

      /*--- Write surface tecplot ascii ---*/
      if (rank == MASTER_NODE) {
        (*fileWritingTable) << "Tecplot ASCII surface" << fileName + extension;

        if (!config->GetWrt_Surface_Overwrite())
          (*fileWritingTable) << "Tecplot ASCII surface + iter" << filename_iter + extension;

      }

      fileWriter = new CTecplotFileWriter(surfaceDataSorter,
                                          curTimeIter, GetHistoryFieldValue("TIME_STEP"));

      break;

    case OUTPUT_TYPE::SURFACE_TECPLOT_BINARY:

      extension = CTecplotBinaryFileWriter::fileExt;

      if (fileName.empty())
        fileName = config->GetFilename(surfaceFilename, "", curTimeIter);

      if (!config->GetWrt_Surface_Overwrite())
        filename_iter = config->GetFilename_Iter(fileName,curInnerIter, curOuterIter);


      /*--- Load and sort the output data and connectivity. ---*/

      surfaceDataSorter->SortConnectivity(config, geometry);
      surfaceDataSorter->SortOutputData();

      /*--- Write surface tecplot binary ---*/
      if (rank == MASTER_NODE) {
        (*fileWritingTable) << "Tecplot binary surface" << fileName + extension;

        if (!config->GetWrt_Surface_Overwrite())
          (*fileWritingTable) << "Tecplot binary surface + iter" << filename_iter + extension;

      }

      fileWriter = new CTecplotBinaryFileWriter(surfaceDataSorter,
                                                curTimeIter, GetHistoryFieldValue("TIME_STEP"));

      break;

    case OUTPUT_TYPE::STL_ASCII:

      extension = CSTLFileWriter::fileExt;

      if (fileName.empty())
        fileName = config->GetFilename(surfaceFilename, "", curTimeIter);

      if (!config->GetWrt_Surface_Overwrite())
        filename_iter = config->GetFilename_Iter(fileName,curInnerIter, curOuterIter);


      /*--- Load and sort the output data and connectivity. ---*/

      surfaceDataSorter->SortConnectivity(config, geometry);
      surfaceDataSorter->SortOutputData();

      /*--- Write ASCII STL ---*/
      if (rank == MASTER_NODE) {
        (*fileWritingTable) << "STL ASCII" << fileName + extension;

        if (!config->GetWrt_Surface_Overwrite())
          (*fileWritingTable) << "STL ASCII + iter" << filename_iter + extension;

      }

      fileWriter = new CSTLFileWriter(surfaceDataSorter);

      break;

    case OUTPUT_TYPE::CGNS:

      extension = CCGNSFileWriter::fileExt;

      if (fileName.empty())
        fileName = config->GetFilename(volumeFilename, "", curTimeIter);

      if (!config->GetWrt_Volume_Overwrite())
        filename_iter = config->GetFilename_Iter(fileName,curInnerIter, curOuterIter);


      /*--- Load and sort the output data and connectivity. ---*/
      volumeDataSorter->SortConnectivity(config, geometry, true);

      /*--- Write CGNS ---*/
      if (rank == MASTER_NODE) {
        (*fileWritingTable) << "CGNS" << fileName + extension;

        if (!config->GetWrt_Volume_Overwrite())
          (*fileWritingTable) << "CGNS + iter" << filename_iter + extension;

      }

      fileWriter = new CCGNSFileWriter(volumeDataSorter);

      break;

    case OUTPUT_TYPE::SURFACE_CGNS:

      extension = CCGNSFileWriter::fileExt;

      if (fileName.empty())
        fileName = config->GetFilename(surfaceFilename, "", curTimeIter);

      if (!config->GetWrt_Surface_Overwrite())
        filename_iter = config->GetFilename_Iter(fileName,curInnerIter, curOuterIter);


      /*--- Load and sort the output data and connectivity. ---*/
      surfaceDataSorter->SortConnectivity(config, geometry);
      surfaceDataSorter->SortOutputData();

      /*--- Write SURFACE_CGNS ---*/
      if (rank == MASTER_NODE) {
        (*fileWritingTable) << "CGNS surface" << fileName + extension;

        if (!config->GetWrt_Surface_Overwrite())
          (*fileWritingTable) << "CGNS surface + iter" << filename_iter + extension;

      }

      fileWriter = new CCGNSFileWriter(surfaceDataSorter, true);

      break;

    default:
      break;
  }

  if (fileWriter != nullptr){

    /*--- Write data to file ---*/

    fileWriter->WriteData(fileName);

    su2double BandWidth = fileWriter->GetBandwidth();

    /*--- Write data with iteration number to file ---*/

    if (!filename_iter.empty() && !config->GetWrt_Restart_Overwrite()){
      fileWriter->WriteData(filename_iter);

      /*--- overwrite bandwidth ---*/
      BandWidth = fileWriter->GetBandwidth();

    }


  /*--- Compute and store the bandwidth ---*/

    if (format == OUTPUT_TYPE::RESTART_BINARY){
      config->SetRestart_Bandwidth_Agg(config->GetRestart_Bandwidth_Agg()+BandWidth);
    }

    if (config->GetWrt_Performance() && (rank == MASTER_NODE)){
      fileWritingTable->SetAlign(PrintingToolbox::CTablePrinter::RIGHT);
      (*fileWritingTable) << " " << "(" + PrintingToolbox::to_string(BandWidth) + " MB/s)";
      fileWritingTable->SetAlign(PrintingToolbox::CTablePrinter::LEFT);
    }

    delete fileWriter;

  }

}

bool COutput::GetCauchyCorrectedTimeConvergence(const CConfig *config){
  if(!cauchyTimeConverged && TimeConvergence && config->GetTime_Marching() == TIME_MARCHING::DT_STEPPING_2ND){
    // Change flags for 2nd order Time stepping: In case of convergence, this iter and next iter gets written out. then solver stops
    cauchyTimeConverged = TimeConvergence;
    TimeConvergence = false;
  }
  else if(cauchyTimeConverged){
    TimeConvergence = cauchyTimeConverged;
  }
  return TimeConvergence;
}

bool COutput::SetResultFiles(CGeometry *geometry, CConfig *config, CSolver** solver_container,
                              unsigned long iter, bool force_writing) {

  bool isFileWrite = false, dataIsLoaded = false;
  const auto nVolumeFiles = config->GetnVolumeOutputFiles();
  const auto* VolumeFiles = config->GetVolumeOutputFiles();

  /*--- Check if the data sorters are allocated, if not, allocate them. --- */
  AllocateDataSorters(config, geometry);

  for (unsigned short iFile = 0; iFile < nVolumeFiles; iFile++) {

    /*--- Collect the volume data from the solvers.
     *  If time-domain is enabled, we also load the data although we don't output it,
     *  since we might want to do time-averaging. ---*/
    const bool write_file = WriteVolumeOutput(config, iter, force_writing || cauchyTimeConverged, iFile);

    if ((write_file || config->GetTime_Domain()) && !dataIsLoaded) {
      LoadDataIntoSorter(config, geometry, solver_container);
      dataIsLoaded = true;
    }
    if (!write_file) continue;

    /*--- Partition and sort the data --- */

    volumeDataSorter->SortOutputData();

    if (rank == MASTER_NODE && !isFileWrite) {
      fileWritingTable->SetAlign(PrintingToolbox::CTablePrinter::CENTER);
      fileWritingTable->PrintHeader();
      fileWritingTable->SetAlign(PrintingToolbox::CTablePrinter::LEFT);
    }

    /*--- Loop through all requested output files and write
     * the partitioned and sorted data stored in the data sorters. ---*/

    WriteToFile(config, geometry, VolumeFiles[iFile]);

    /*--- Write any additonal files defined in the child class ----*/

    WriteAdditionalFiles(config, geometry, solver_container);

    isFileWrite = true;
  }

  if (rank == MASTER_NODE && isFileWrite) {
    fileWritingTable->PrintFooter();
    headerNeeded = true;
  }

  return isFileWrite;
}

void COutput::PrintConvergenceSummary(){

  PrintingToolbox::CTablePrinter  ConvSummary(&cout);

  ConvSummary.AddColumn("Convergence Field", 28);
  ConvSummary.AddColumn("Value", 14);
  ConvSummary.AddColumn("Criterion", 14);
  ConvSummary.AddColumn("Converged",12);
  ConvSummary.SetAlign(PrintingToolbox::CTablePrinter::CENTER);
  ConvSummary.PrintHeader();
  for (unsigned short iField_Conv = 0; iField_Conv < convFields.size(); iField_Conv++){
    const string &convField = convFields[iField_Conv];
    if (historyOutput_Map.at(convField).fieldType == HistoryFieldType::COEFFICIENT) {
      string convMark = "No";
      if ( historyOutput_Map.at("CAUCHY_" + convField).value < cauchyEps) convMark = "Yes";
      ConvSummary << historyOutput_Map.at("CAUCHY_" + convField).fieldName
          <<  historyOutput_Map.at("CAUCHY_" + convField).value
          << " < " + PrintingToolbox::to_string(cauchyEps) << convMark;
    }
    else if (historyOutput_Map.at(convField).fieldType == HistoryFieldType::RESIDUAL ||
        historyOutput_Map.at(convField).fieldType == HistoryFieldType::AUTO_RESIDUAL)  {
      string convMark = "No";
      if (historyOutput_Map.at(convField).value < minLogResidual) convMark = "Yes";
      ConvSummary << historyOutput_Map.at(convField).fieldName
          << historyOutput_Map.at(convField).value
          << " < " + PrintingToolbox::to_string(minLogResidual) << convMark;
    }
  }
  ConvSummary.PrintFooter();
}

bool COutput::ConvergenceMonitoring(CConfig *config, unsigned long Iteration) {

  convergence = true;

  for (auto iField_Conv = 0ul; iField_Conv < convFields.size(); iField_Conv++) {

    const auto& convField = convFields[iField_Conv];
    const auto it = historyOutput_Map.find(convField);

    if (it == historyOutput_Map.end()) continue;

    const auto& field = it->second;
    const su2double monitor = field.value;

    /*--- Stop the simulation in case a nan appears, do not save the solution. ---*/
    if (std::isnan(SU2_TYPE::GetValue(monitor))) {
      SU2_MPI::Error("SU2 has diverged (NaN detected).", CURRENT_FUNCTION);
    }

    bool fieldConverged = false;

    switch (field.fieldType) {

      /*--- Cauchy based convergence criteria ---*/
      case HistoryFieldType::COEFFICIENT: {

        if (Iteration == 0) {
          for (auto iCounter = 0ul; iCounter < nCauchy_Elems; iCounter++) {
            cauchySerie[iField_Conv][iCounter] = 0.0;
          }
          newFunc[iField_Conv] = monitor;
        }

        oldFunc[iField_Conv] = newFunc[iField_Conv];
        newFunc[iField_Conv] = monitor;
        /*--- Automatically modify the scaling factor of relative Cauchy convergence for
        * coefficients that are close to zero. Example: For the clean aircraft, the rolling
        * moment coefficient MOMENT_X is close to zero and thus will never reach a relative
        * cauchy convergence ->> dividing tiny numbers is not a good idea. Using absolute
        * cauchy convergence is more robust in this case. ---*/
        cauchyFunc = fabs(newFunc[iField_Conv] - oldFunc[iField_Conv]) / fmax(fabs(monitor), 0.1);

        cauchySerie[iField_Conv][Iteration % nCauchy_Elems] = cauchyFunc;
        cauchyValue = 0.0;
        for (auto iCounter = 0ul; iCounter < nCauchy_Elems; iCounter++)
          cauchyValue += cauchySerie[iField_Conv][iCounter];

        cauchyValue /= nCauchy_Elems;

        /*--- Start monitoring only if the current iteration
         *    is larger than the number of cauchy elements. --- */
        fieldConverged = (cauchyValue < cauchyEps) && (Iteration >= nCauchy_Elems);

        if (Iteration == 0) cauchyValue = 1.0;
        SetHistoryOutputValue("CAUCHY_" + convField, cauchyValue);

      } break;


      /*--- Residual based convergence criteria ---*/
      case HistoryFieldType::RESIDUAL:
      case HistoryFieldType::AUTO_RESIDUAL:

        fieldConverged = (Iteration != 0) && (monitor <= minLogResidual);
        break;

      default:
        break;
    }

    convergence = fieldConverged && convergence;
  }

  /*--- Do not apply any convergence criteria if the number
   *    of iterations is less than a particular value. ---*/

  if (convFields.empty() || Iteration < config->GetStartConv_Iter()) convergence = false;

  /*--- Apply the same convergence criteria to all processors. ---*/

  unsigned short local = convergence, global = 0;
  SU2_MPI::Allreduce(&local, &global, 1, MPI_UNSIGNED_SHORT, MPI_MAX, SU2_MPI::GetComm());
  convergence = global > 0;

  return convergence;
}

bool COutput::MonitorTimeConvergence(CConfig *config, unsigned long TimeIteration) {

  bool Inner_IterConv = GetConvergence() || config->GetnInner_Iter()-1 <= curInnerIter; //Check, if Inner_Iter is converged

  if(TimeIteration == 0){
    for (unsigned short iField_Conv = 0; iField_Conv < wndConvFields.size(); iField_Conv++){
      const string WndConv_Field= wndConvFields[iField_Conv];
      if (historyOutput_Map.count(WndConv_Field) > 0){
        SetHistoryOutputValue("CAUCHY_"+ WndConv_Field, 1.0);
      }
    }
  }
  if(Inner_IterConv && TimeIteration >= config->GetStartWindowIteration()){
    TimeConvergence = true;
    unsigned short iCounter;

    for (unsigned short iField_Conv = 0; iField_Conv < wndConvFields.size(); iField_Conv++){
      bool fieldConverged = false;
      const string WndConv_Field= wndConvFields[iField_Conv];

      if (historyOutput_Map.count(WndConv_Field) > 0){
        su2double monitor = historyOutput_Map[WndConv_Field].value;

        /*--- Stop the simulation in case a nan appears, do not save the solution ---*/
        if (std::isnan(SU2_TYPE::GetValue(monitor))) {
          SU2_MPI::Error("SU2 has diverged (NaN detected).", CURRENT_FUNCTION);
        }

        /*--- Cauchy based convergence criteria ---*/

        if (historyOutput_Map[WndConv_Field].fieldType == HistoryFieldType::AUTO_COEFFICIENT) { //TAVG values are AUTO_COEFF
          if (TimeIteration == config->GetStartWindowIteration()){
            for (iCounter = 0; iCounter < nWndCauchy_Elems; iCounter++){
              WndCauchy_Serie[iField_Conv][iCounter] = 0.0;
            }
            WndNew_Func[iField_Conv] = monitor;
          }
          WndOld_Func[iField_Conv] = WndNew_Func[iField_Conv];
          WndNew_Func[iField_Conv] = monitor;
          WndCauchy_Func = fabs(WndNew_Func[iField_Conv] - WndOld_Func[iField_Conv]);
          WndCauchy_Serie[iField_Conv][TimeIteration % nWndCauchy_Elems] = WndCauchy_Func;
          WndCauchy_Value = 1.0;

          if (TimeIteration >= nWndCauchy_Elems+config->GetStartWindowIteration()){
            WndCauchy_Value = 0.0;
            for (iCounter = 0; iCounter < nWndCauchy_Elems; iCounter++){
              WndCauchy_Value += WndCauchy_Serie[iField_Conv][iCounter];
            }
            WndCauchy_Value /= nWndCauchy_Elems;
          }
          if (WndCauchy_Value >= wndCauchyEps){fieldConverged = false;}
          else{fieldConverged = true;}

          /*--- Start monitoring only if the current iteration is larger than the
           *  number of cauchy elements and the number of start-up iterations ---*/

          if (TimeIteration <  config->GetStartWindowIteration() + max(config->GetWnd_StartConv_Iter(), nWndCauchy_Elems)){
            fieldConverged = false;
          }
          SetHistoryOutputValue("CAUCHY_" + WndConv_Field, WndCauchy_Value);
        }
        TimeConvergence = fieldConverged && TimeConvergence;
      }
    }

    /*--- Do not apply any convergence criterion if the option is disabled. */
    if(!config->GetWnd_Cauchy_Crit()){TimeConvergence = false;}
    if(wndConvFields.empty()){TimeConvergence = false;}
  }
  return TimeConvergence;
}

void COutput::SetHistoryFileHeader(const CConfig *config) {

  unsigned short iField_Output = 0,
      iReqField = 0,
      iMarker = 0;
  stringstream out;
  int width = 20;

  for (iField_Output = 0; iField_Output < historyOutput_List.size(); iField_Output++){
    const string &fieldIdentifier = historyOutput_List[iField_Output];
    const HistoryOutputField &field = historyOutput_Map.at(fieldIdentifier);
    for (iReqField = 0; iReqField < nRequestedHistoryFields; iReqField++){
      const string & requestedField = requestedHistoryFields[iReqField];
      if (requestedField == field.outputGroup || (requestedField == fieldIdentifier)){
        if (field.screenFormat == ScreenOutputFormat::INTEGER) width = std::max((int)field.fieldName.size()+2, 10);
        else{ width = std::max((int)field.fieldName.size()+2, 18);}
        historyFileTable->AddColumn("\"" + field.fieldName + "\"", width);
      }
    }
  }

  for (iField_Output = 0; iField_Output < historyOutputPerSurface_List.size(); iField_Output++){
    const string &fieldIdentifier = historyOutputPerSurface_List[iField_Output];
    for (iMarker = 0; iMarker < historyOutputPerSurface_Map[fieldIdentifier].size(); iMarker++){
      const HistoryOutputField &field = historyOutputPerSurface_Map.at(fieldIdentifier)[iMarker];
      for (iReqField = 0; iReqField < nRequestedHistoryFields; iReqField++){
        const string &requestedField = requestedHistoryFields[iReqField];
        if (requestedField == field.outputGroup || (requestedField == fieldIdentifier)){
          if (field.screenFormat == ScreenOutputFormat::INTEGER) width = std::max((int)field.fieldName.size()+2, 10);
          else{ width = std::max((int)field.fieldName.size()+2, 18);}
          historyFileTable->AddColumn("\"" + field.fieldName + "\"", width);
        }
      }
    }
  }

  if (config->GetTabular_FileFormat() == TAB_OUTPUT::TAB_TECPLOT) {
    histFile << "VARIABLES = \\" << endl;
  }
  historyFileTable->PrintHeader();
  histFile.flush();
}


void COutput::SetHistoryFileOutput(const CConfig *config) {

  if (requestedHistoryFieldCache.empty()) {
    for (const auto& fieldIdentifier : historyOutput_List){
      const auto& field = historyOutput_Map.at(fieldIdentifier);
      for (const auto& requestedField : requestedHistoryFields) {
        if ((requestedField == field.outputGroup) || (requestedField == fieldIdentifier)) {
          requestedHistoryFieldCache.push_back(&field.value);
        }
      }
    }

    for (const auto& fieldIdentifier : historyOutputPerSurface_List) {
      for (const auto& field : historyOutputPerSurface_Map.at(fieldIdentifier)) {
        for (const auto& requestedField : requestedHistoryFields){
          if ((requestedField == field.outputGroup) || (requestedField == fieldIdentifier)) {
            requestedHistoryFieldCache.push_back(&field.value);
          }
        }
      }
    }
  }

  for (const auto* valPtr : requestedHistoryFieldCache) {
    (*historyFileTable) << *valPtr;
  }

  /*--- Print the string to file and remove the last two characters (a separator and a space) ---*/

  histFile.flush();
}

void COutput::SetScreenHeader(const CConfig *config) {
  if (config->GetMultizone_Problem())
    multiZoneHeaderTable->PrintHeader();
  convergenceTable->PrintHeader();
}


void COutput::SetScreenOutput(const CConfig *config) {

  if (requestedScreenFieldCache.empty()) {
    for (const auto& RequestedField : requestedScreenFields) {
      const auto it1 = historyOutput_Map.find(RequestedField);
      if (it1 != historyOutput_Map.end()) {
        requestedScreenFieldCache.push_back(&it1->second);
      }
      const auto it2 = historyOutputPerSurface_Map.find(RequestedField);
      if (it2 != historyOutputPerSurface_Map.end()) {
        for (size_t i = 0; i < it2->second.size(); ++i) {
          requestedScreenFieldCache.push_back(&it2->second[i]);
        }
      }
    }
  }

  for (const auto* fieldPtr : requestedScreenFieldCache) {
    const auto& field = *fieldPtr;
    stringstream out;
    switch (field.screenFormat) {
      case ScreenOutputFormat::INTEGER:
        PrintingToolbox::PrintScreenInteger(out, SU2_TYPE::Int(field.value), fieldWidth);
        break;
      case ScreenOutputFormat::FIXED:
        PrintingToolbox::PrintScreenFixed(out, field.value, fieldWidth);
        break;
      case ScreenOutputFormat::SCIENTIFIC:
        PrintingToolbox::PrintScreenScientific(out, field.value, fieldWidth);
        break;
      case ScreenOutputFormat::PERCENT:
        PrintingToolbox::PrintScreenPercent(out, field.value, fieldWidth);
        break;
    }
    (*convergenceTable) << out.str();
  }

  SetAdditionalScreenOutput(config);
}

void COutput::PreprocessHistoryOutput(CConfig *config, bool wrt){

  noWriting = !wrt;

  /*--- Set the common output fields ---*/

  SetCommonHistoryFields();

  /*--- Set the History output fields using a virtual function call to the child implementation ---*/

  SetHistoryOutputFields(config);

  /*--- Detect user-defined outputs ---*/

  SetCustomOutputs(config);

  /*--- Postprocess the history fields. Creates new fields based on the ones set in the child classes ---*/

  PostprocessHistoryFields(config);

  /*--- We use a fixed size of the file output summary table ---*/

  int total_width = 72;
  fileWritingTable->AddColumn("File Writing Summary", (total_width)/2-1);
  fileWritingTable->AddColumn("Filename", total_width/2-1);
  fileWritingTable->SetAlign(PrintingToolbox::CTablePrinter::LEFT);

  /*--- Check for consistency and remove fields that are requested but not available --- */

  CheckHistoryOutput();

  if (rank == MASTER_NODE && !noWriting){

    /*--- Open history file and print the header ---*/
    if (!config->GetMultizone_Problem() || config->GetWrt_ZoneHist())
      PrepareHistoryFile(config);

    total_width = nRequestedScreenFields*fieldWidth + (nRequestedScreenFields-1);

    /*--- Set the multizone screen header ---*/

    if (config->GetMultizone_Problem()){
      multiZoneHeaderTable->AddColumn(multiZoneHeaderString, total_width);
      multiZoneHeaderTable->SetAlign(PrintingToolbox::CTablePrinter::CENTER);
      multiZoneHeaderTable->SetPrintHeaderBottomLine(false);
    }

  }

}

void COutput::PreprocessMultizoneHistoryOutput(COutput **output, CConfig **config, CConfig* driver_config, bool wrt){

  noWriting = !wrt;

  /*--- Set the common history fields for all solvers ---*/

  SetCommonHistoryFields();

  /*--- Set the History output fields using a virtual function call to the child implementation ---*/

  SetMultizoneHistoryOutputFields(output, config);

  /*--- Postprocess the history fields. Creates new fields based on the ones set in the child classes ---*/

  PostprocessHistoryFields(driver_config);

  /*--- We use a fixed size of the file output summary table ---*/

  int total_width = 72;
  fileWritingTable->AddColumn("File Writing Summary", (total_width-1)/2);
  fileWritingTable->AddColumn("Filename", total_width/2);
  fileWritingTable->SetAlign(PrintingToolbox::CTablePrinter::LEFT);

  /*--- Check for consistency and remove fields that are requested but not available --- */

  CheckHistoryOutput();

  if (rank == MASTER_NODE && !noWriting){

    /*--- Open history file and print the header ---*/

    PrepareHistoryFile(driver_config);

    total_width = nRequestedScreenFields*fieldWidth + (nRequestedScreenFields-1);

    /*--- Set the multizone screen header ---*/

    if (config[ZONE_0]->GetMultizone_Problem()){
      multiZoneHeaderTable->AddColumn(multiZoneHeaderString, nRequestedScreenFields*fieldWidth + (nRequestedScreenFields-1));
      multiZoneHeaderTable->SetAlign(PrintingToolbox::CTablePrinter::CENTER);
      multiZoneHeaderTable->SetPrintHeaderBottomLine(false);
    }

  }

}

void COutput::PrepareHistoryFile(CConfig *config){

  /*--- Open the history file ---*/

  histFile.open(historyFilename, ios::out);

  /*--- Create and format the history file table ---*/

  historyFileTable->SetInnerSeparator(historySep);
  historyFileTable->SetAlign(PrintingToolbox::CTablePrinter::CENTER);
  historyFileTable->SetPrintHeaderTopLine(false);
  historyFileTable->SetPrintHeaderBottomLine(false);
  historyFileTable->SetPrecision(config->GetOutput_Precision());

  /*--- Add the header to the history file. ---*/

  SetHistoryFileHeader(config);

}

void COutput::CheckHistoryOutput() {

  /*--- Set screen convergence output header and remove unavailable fields ---*/

  vector<string> requestWithExpandedGroups;

  for (const auto& requestedField : requestedScreenFields) {
    bool isGroup = false;
    for (const auto& name : historyOutput_List) {
      if (requestedField == historyOutput_Map.at(name).outputGroup) {
        isGroup = true;
        requestWithExpandedGroups.push_back(name);
      }
    }
    for (const auto& name : historyOutputPerSurface_List) {
      if (requestedField == historyOutputPerSurface_Map.at(name).front().outputGroup) {
        isGroup = true;
        requestWithExpandedGroups.push_back(name);
      }
    }
    if (!isGroup) {
      requestWithExpandedGroups.push_back(requestedField);
    }
  }
  requestedScreenFields = std::move(requestWithExpandedGroups);

  vector<string> FieldsToRemove;

  for (const auto& requestedField : requestedScreenFields) {
    const auto it1 = historyOutput_Map.find(requestedField);
    if (it1 != historyOutput_Map.end()) {
      convergenceTable->AddColumn(it1->second.fieldName, fieldWidth);
    }
    const auto it2 = historyOutputPerSurface_Map.find(requestedField);
    if (it2 != historyOutputPerSurface_Map.end()) {
      for (const auto& field : it2->second) {
        convergenceTable->AddColumn(field.fieldName, fieldWidth);
      }
    }
    if (it1 == historyOutput_Map.end() && it2 == historyOutputPerSurface_Map.end()) {
      FieldsToRemove.push_back(requestedField);
    }
  }

  /*--- Remove fields which are not defined --- */

  for (unsigned short iReqField = 0; iReqField < FieldsToRemove.size(); iReqField++) {
    if (rank == MASTER_NODE) {
      if (iReqField == 0) {
        cout << "  Info: Ignoring the following screen output fields:\n  ";
      }
      cout << FieldsToRemove[iReqField];
      if (iReqField != FieldsToRemove.size()-1) {
        cout << ", ";
      } else {
        cout << endl;
      }
    }
    requestedScreenFields.erase(std::find(requestedScreenFields.begin(),
                                          requestedScreenFields.end(), FieldsToRemove[iReqField]));
  }

  nRequestedScreenFields = requestedScreenFields.size();

  if (rank == MASTER_NODE){
    cout <<"Screen output fields: ";
    for (unsigned short iReqField = 0; iReqField < nRequestedScreenFields; iReqField++){
      cout << requestedScreenFields[iReqField];
      if (iReqField != nRequestedScreenFields - 1) cout << ", ";
    }
    cout << endl;
  }

  /*--- Remove unavailable fields from the history file output ---*/

  FieldsToRemove.clear();
  vector<bool> FoundField(nRequestedHistoryFields, false);

  for (const auto& fieldReference : historyOutput_List) {
    const auto &field = historyOutput_Map.at(fieldReference);
    for (unsigned short iReqField = 0; iReqField < nRequestedHistoryFields; iReqField++) {
      const auto& requestedField = requestedHistoryFields[iReqField];
      if ((requestedField == field.outputGroup) || (requestedField == fieldReference)) {
        FoundField[iReqField] = true;
      }
    }
  }

  for (const auto& fieldReference : historyOutputPerSurface_List) {
    for (const auto& field : historyOutputPerSurface_Map.at(fieldReference)) {
      for (unsigned short iReqField = 0; iReqField < nRequestedHistoryFields; iReqField++){
        const auto& requestedField = requestedHistoryFields[iReqField];
        if ((requestedField == field.outputGroup) || (requestedField == fieldReference)) {
          FoundField[iReqField] = true;
        }
      }
    }
  }

  for (unsigned short iReqField = 0; iReqField < nRequestedHistoryFields; iReqField++){
    if (!FoundField[iReqField]){
      FieldsToRemove.push_back(requestedHistoryFields[iReqField]);
    }
  }

  /*--- Remove fields which are not defined --- */

  for (unsigned short iReqField = 0; iReqField < FieldsToRemove.size(); iReqField++){
    if (rank == MASTER_NODE) {
      if (iReqField == 0){
        cout << "  Info: Ignoring the following history output groups:\n  ";
      }
      cout << FieldsToRemove[iReqField];
      if (iReqField != FieldsToRemove.size()-1){
        cout << ", ";
      } else {
        cout << endl;
      }
    }
    requestedHistoryFields.erase(std::find(requestedHistoryFields.begin(),
                                           requestedHistoryFields.end(), FieldsToRemove[iReqField]));
  }

  nRequestedHistoryFields = requestedHistoryFields.size();

  if (rank == MASTER_NODE){
    cout <<"History output group(s): ";
    for (unsigned short iReqField = 0; iReqField < nRequestedHistoryFields; iReqField++){
      cout << requestedHistoryFields[iReqField];
      if (iReqField != nRequestedHistoryFields - 1) cout << ", ";
    }
    cout << endl;
  }

  /*--- Check that the requested convergence monitoring field is available ---*/
  bool removedField = false;
  FieldsToRemove.clear();
  for (unsigned short iField_Conv = 0; iField_Conv < convFields.size(); iField_Conv++){
    if (historyOutput_Map.count(convFields[iField_Conv]) == 0){
      if (!removedField) {
        if(rank == MASTER_NODE) cout << "Ignoring Convergence Field(s): ";
        removedField = true;
      }
      if(rank == MASTER_NODE) cout << convFields[iField_Conv] << " ";
      FieldsToRemove.push_back(convFields[iField_Conv]);
    }
  }
  if (removedField && (rank == MASTER_NODE)) cout << endl;
  for (unsigned short iField_Conv = 0; iField_Conv < FieldsToRemove.size(); iField_Conv++){
    convFields.erase(std::find(convFields.begin(),
                               convFields.end(), FieldsToRemove[iField_Conv]));
  }

  if (rank == MASTER_NODE){
    if(convFields.empty()){
      cout << "Warning: No (valid) fields chosen for convergence monitoring. Convergence monitoring inactive."<<  endl;
    }
    else{
      cout <<"Convergence field(s): ";
      for (unsigned short iField_Conv = 0; iField_Conv < convFields.size(); iField_Conv++){
        cout << convFields[iField_Conv];
        if (iField_Conv != convFields.size() - 1) cout << ", ";
      }
      cout << endl;
    }
  }

  /*--- Check that the requested time convergene monitoring field(s) are available*/
  removedField = false;
  FieldsToRemove.clear();
  for (unsigned short iField_Conv = 0; iField_Conv < wndConvFields.size(); iField_Conv++){
    if (historyOutput_Map.count(wndConvFields[iField_Conv]) == 0){
      if (!removedField) {
        if(rank == MASTER_NODE) cout << "Ignoring Time Convergence Field(s): ";
        removedField = true;
      }
      if(rank == MASTER_NODE)cout << wndConvFields[iField_Conv] << " ";
      FieldsToRemove.push_back(wndConvFields[iField_Conv]);
    }
  }
  if (removedField && rank ==MASTER_NODE) cout << endl;

  for (unsigned short iField_Conv = 0; iField_Conv < FieldsToRemove.size(); iField_Conv++){
    wndConvFields.erase(std::find(wndConvFields.begin(), wndConvFields.end(), FieldsToRemove[iField_Conv]));
  }
  if (rank == MASTER_NODE){
    if(wndConvFields.empty()){
      cout << "Warning: No (valid) fields chosen for time convergence monitoring. Time convergence monitoring inactive."<<  endl;
    }
    else{
      cout <<"Time Convergence field(s): ";
      for (unsigned short iField_Conv = 0; iField_Conv < wndConvFields.size(); iField_Conv++){
        cout << wndConvFields[iField_Conv];
        if (iField_Conv != wndConvFields.size() - 1) cout << ", ";
      }
      cout << endl;
    }
  }
}

void COutput::PreprocessVolumeOutput(CConfig *config){

  /*--- Set the volume output fields using a virtual function call to the child implementation ---*/

  SetVolumeOutputFields(config);

  /*---Coordinates and solution groups must be always in the output.
   * If they are not requested, add them here. ---*/

  vector<string>::iterator itCoord = std::find(requestedVolumeFields.begin(),
                                          requestedVolumeFields.end(), "COORDINATES");
  if (itCoord == requestedVolumeFields.end()){
    requestedVolumeFields.emplace_back("COORDINATES");
    nRequestedVolumeFields++;
  }
  vector<string>::iterator itSol = std::find(requestedVolumeFields.begin(),
                                          requestedVolumeFields.end(), "SOLUTION");
  if (itSol == requestedVolumeFields.end()){
    requestedVolumeFields.emplace_back("SOLUTION");
    nRequestedVolumeFields++;
  }

  nVolumeFields = 0;

  string RequestedField;
  std::vector<bool> FoundField(nRequestedVolumeFields, false);
  vector<string> FieldsToRemove;


  /*--- Loop through all fields defined in the corresponding SetVolumeOutputFields().
 * If it is also defined in the config (either as part of a group or a single field), the field
 * object gets an offset so that we know where to find the data in the Local_Data() array.
 *  Note that the default offset is -1. An index !=-1 defines this field as part of the output. ---*/

  for (unsigned short iField_Output = 0; iField_Output < volumeOutput_List.size(); iField_Output++){

    const string &fieldReference = volumeOutput_List[iField_Output];
    if (volumeOutput_Map.count(fieldReference) > 0){
      VolumeOutputField &Field = volumeOutput_Map.at(fieldReference);

      /*--- Loop through all fields specified in the config ---*/

      for (unsigned short iReqField = 0; iReqField < nRequestedVolumeFields; iReqField++){

        RequestedField = requestedVolumeFields[iReqField];

        if (((RequestedField == Field.outputGroup) || (RequestedField == fieldReference)) && (Field.offset == -1)){
          Field.offset = nVolumeFields;
          volumeFieldNames.push_back(Field.fieldName);
          nVolumeFields++;

          FoundField[iReqField] = true;
        }
      }
    }
  }

  for (unsigned short iReqField = 0; iReqField < nRequestedVolumeFields; iReqField++){
    if (!FoundField[iReqField]){
      FieldsToRemove.push_back(requestedVolumeFields[iReqField]);
    }
  }

  /*--- Remove fields which are not defined --- */

  for (unsigned short iReqField = 0; iReqField < FieldsToRemove.size(); iReqField++){
    if (rank == MASTER_NODE) {
      if (iReqField == 0){
        cout << "  Info: Ignoring the following volume output fields/groups:" << endl;
        cout << "  ";
      }
      cout << FieldsToRemove[iReqField];
      if (iReqField != FieldsToRemove.size()-1){
        cout << ", ";
      } else {
        cout << endl;
      }
    }
    requestedVolumeFields.erase(std::find(requestedVolumeFields.begin(),
                                          requestedVolumeFields.end(), FieldsToRemove[iReqField]));
  }

  nRequestedVolumeFields = requestedVolumeFields.size();

  if (rank == MASTER_NODE){
    cout <<"Volume output fields: ";
    for (unsigned short iReqField = 0; iReqField < nRequestedVolumeFields; iReqField++){
      RequestedField = requestedVolumeFields[iReqField];
      cout << requestedVolumeFields[iReqField];
      if (iReqField != nRequestedVolumeFields - 1) cout << ", ";
    }
    cout << endl;
  }
}

void COutput::LoadDataIntoSorter(CConfig* config, CGeometry* geometry, CSolver** solver){

  unsigned short iMarker = 0;
  unsigned long iPoint = 0, jPoint = 0;
  unsigned long iVertex = 0;

  /*--- Reset the offset cache and index --- */
  cachePosition = 0;
  fieldIndexCache.clear();
  curGetFieldIndex = 0;
  fieldGetIndexCache.clear();

  if (femOutput){

    /*--- Create an object of the class CMeshFEM_DG and retrieve the necessary
     geometrical information for the FEM DG solver. ---*/

    CMeshFEM_DG *DGGeometry = dynamic_cast<CMeshFEM_DG *>(geometry);

    unsigned long nVolElemOwned = DGGeometry->GetNVolElemOwned();

    CVolumeElementFEM *volElem  = DGGeometry->GetVolElem();

    /*--- Access the solution by looping over the owned volume elements. ---*/

    for(unsigned long l=0; l<nVolElemOwned; ++l) {

      for(unsigned short j=0; j<volElem[l].nDOFsSol; ++j) {

        buildFieldIndexCache = fieldIndexCache.empty();

        LoadVolumeDataFEM(config, geometry, solver, l, jPoint, j);

        jPoint++;

      }
    }

  } else {

    for (iPoint = 0; iPoint < geometry->GetnPointDomain(); iPoint++) {

      /*--- Load the volume data into the data sorter. --- */

      buildFieldIndexCache = fieldIndexCache.empty();

      LoadVolumeData(config, geometry, solver, iPoint);

    }

    /*--- Reset the offset cache and index --- */
    cachePosition = 0;
    fieldIndexCache.clear();
    curGetFieldIndex = 0;
    fieldGetIndexCache.clear();

    for (iMarker = 0; iMarker < config->GetnMarker_All(); iMarker++) {

      /*--- We only want to have surface values on solid walls ---*/

      if (config->GetSolid_Wall(iMarker)){
        for (iVertex = 0; iVertex < geometry->GetnVertex(iMarker); iVertex++){

          iPoint = geometry->vertex[iMarker][iVertex]->GetNode();

          /*--- Load the surface data into the data sorter. --- */

          if(geometry->nodes->GetDomain(iPoint)){

            buildFieldIndexCache = fieldIndexCache.empty();

            LoadSurfaceData(config, geometry, solver, iPoint, iMarker, iVertex);

          }
        }
      }
    }
  }
}

void COutput::SetVolumeOutputValue(string name, unsigned long iPoint, su2double value){

  if (buildFieldIndexCache){

    /*--- Build up the offset cache to speed up subsequent
     * calls of this routine since the order of calls is
     * the same for every value of iPoint --- */

    if (volumeOutput_Map.count(name) > 0){
      const short Offset = volumeOutput_Map.at(name).offset;
      fieldIndexCache.push_back(Offset);
      if (Offset != -1){
        volumeDataSorter->SetUnsortedData(iPoint, Offset, value);
      }
    } else {
      SU2_MPI::Error(string("Cannot find output field with name ") + name, CURRENT_FUNCTION);
    }
  } else {

    /*--- Use the offset cache for the access ---*/

    const short Offset = fieldIndexCache[cachePosition++];
    if (Offset != -1){
      volumeDataSorter->SetUnsortedData(iPoint, Offset, value);
    }
    if (cachePosition == fieldIndexCache.size()){
      cachePosition = 0;
    }
  }

}

su2double COutput::GetVolumeOutputValue(string name, unsigned long iPoint){

  if (buildFieldIndexCache){

    /*--- Build up the offset cache to speed up subsequent
     * calls of this routine since the order of calls is
     * the same for every value of iPoint --- */

    if (volumeOutput_Map.count(name) > 0){
      const short Offset = volumeOutput_Map.at(name).offset;
      fieldGetIndexCache.push_back(Offset);
      if (Offset != -1){
        return volumeDataSorter->GetUnsortedData(iPoint, Offset);
      }
    } else {
      SU2_MPI::Error(string("Cannot find output field with name ") + name, CURRENT_FUNCTION);
    }
  } else {

    /*--- Use the offset cache for the access ---*/

    const short Offset = fieldGetIndexCache[curGetFieldIndex++];

    if (curGetFieldIndex == fieldGetIndexCache.size()){
      curGetFieldIndex = 0;
    }
    if (Offset != -1){
      return volumeDataSorter->GetUnsortedData(iPoint, Offset);
    }
  }

  return 0.0;
}

void COutput::SetAvgVolumeOutputValue(string name, unsigned long iPoint, su2double value){

  const su2double scaling = 1.0 / su2double(curAbsTimeIter + 1);

  if (buildFieldIndexCache){

    /*--- Build up the offset cache to speed up subsequent
     * calls of this routine since the order of calls is
     * the same for every value of iPoint --- */

    if (volumeOutput_Map.count(name) > 0){
      const short Offset = volumeOutput_Map.at(name).offset;
      fieldIndexCache.push_back(Offset);
      if (Offset != -1){

        const su2double old_value = volumeDataSorter->GetUnsortedData(iPoint, Offset);
        const su2double new_value = value * scaling + old_value *( 1.0 - scaling);

        volumeDataSorter->SetUnsortedData(iPoint, Offset, new_value);
      }
    } else {
      SU2_MPI::Error(string("Cannot find output field with name ") + name, CURRENT_FUNCTION);
    }
  } else {

    /*--- Use the offset cache for the access ---*/

    const short Offset = fieldIndexCache[cachePosition++];
    if (Offset != -1){

      const su2double old_value = volumeDataSorter->GetUnsortedData(iPoint, Offset);
      const su2double new_value = value * scaling + old_value *( 1.0 - scaling);

      volumeDataSorter->SetUnsortedData(iPoint, Offset, new_value);
    }
    if (cachePosition == fieldIndexCache.size()){
      cachePosition = 0;
    }
  }

}

void COutput::PostprocessHistoryData(CConfig *config){

  map<string, pair<su2double, int> > Average;
  map<string, int> Count;

  for (unsigned short iField = 0; iField < historyOutput_List.size(); iField++){
    const string &fieldIdentifier = historyOutput_List[iField];
    const HistoryOutputField &currentField = historyOutput_Map.at(fieldIdentifier);
    if (currentField.fieldType == HistoryFieldType::RESIDUAL){
      if ( SetInitResiduals(config) || (currentField.value > initialResiduals[fieldIdentifier]) ) {
        initialResiduals[fieldIdentifier] = currentField.value;
      }
      SetHistoryOutputValue("REL_" + fieldIdentifier,
                            currentField.value - initialResiduals[fieldIdentifier]);

      Average[currentField.outputGroup].first += currentField.value;
      Average[currentField.outputGroup].second++;

    }

    if (currentField.fieldType == HistoryFieldType::COEFFICIENT){
      if (config->GetTime_Domain()){
        auto it = windowedTimeAverages.find(fieldIdentifier);
        if (it == windowedTimeAverages.end()) {
          it = windowedTimeAverages.insert({fieldIdentifier, CWindowedAverage(config->GetKindWindow())}).first;
        }
        auto& timeAverage = it->second;
        timeAverage.AddValue(currentField.value,config->GetTimeIter(), config->GetStartWindowIteration()); //Collecting Values for Windowing
        SetHistoryOutputValue("TAVG_" + fieldIdentifier, timeAverage.GetVal());
        if (config->GetDirectDiff() != NO_DERIVATIVE) {
          SetHistoryOutputValue("D_TAVG_" + fieldIdentifier, SU2_TYPE::GetDerivative(timeAverage.GetVal()));
        }
      }
      if (config->GetDirectDiff() != NO_DERIVATIVE){
        SetHistoryOutputValue("D_" + fieldIdentifier, SU2_TYPE::GetDerivative(currentField.value));
      }
    }
  }

  map<string, pair<su2double, int> >::iterator it = Average.begin();
  for (it = Average.begin(); it != Average.end(); it++){
    const su2double& value = it->second.first;
    const int& count = it->second.second;
    const su2double average = value/count;
    if (historyOutput_Map.count("AVG_" + it->first) > 0 )
      SetHistoryOutputValue("AVG_" + it->first, average);
  }
}

void COutput::PostprocessHistoryFields(CConfig *config){

  map<string, bool> Average;
  map<string, string> AverageGroupName = {{"BGS_RES", "bgs"},{"RMS_RES","rms"},{"MAX_RES", "max"}};

  for (unsigned short iField = 0; iField < historyOutput_List.size(); iField++){
    const string &fieldIdentifier = historyOutput_List[iField];
    const HistoryOutputField &currentField = historyOutput_Map.at(fieldIdentifier);
    if (currentField.fieldType == HistoryFieldType::RESIDUAL){
      AddHistoryOutput("REL_" + fieldIdentifier, "rel" + currentField.fieldName, currentField.screenFormat,
                       "REL_" + currentField.outputGroup,  "Relative residual.", HistoryFieldType::AUTO_RESIDUAL);
      Average[currentField.outputGroup] = true;
    }
  }

  map<string, bool>::iterator it = Average.begin();
  for (it = Average.begin(); it != Average.end(); it++){
    if (AverageGroupName.count(it->first) > 0) {
      AddHistoryOutput("AVG_" + it->first, "avg[" + AverageGroupName[it->first] + "]", ScreenOutputFormat::FIXED,
          "AVG_" + it->first , "Average residual over all solution variables.", HistoryFieldType::AUTO_RESIDUAL);
    }
  }

  if (config->GetTime_Domain()){
    for (unsigned short iField = 0; iField < historyOutput_List.size(); iField++){
      const string &fieldIdentifier = historyOutput_List[iField];
      const HistoryOutputField &currentField = historyOutput_Map.at(fieldIdentifier);
      if (currentField.fieldType == HistoryFieldType::COEFFICIENT){
        AddHistoryOutput("TAVG_"   + fieldIdentifier, "tavg["  + currentField.fieldName + "]",
                         currentField.screenFormat, "TAVG_"   + currentField.outputGroup, "Time averaged values.",
                         HistoryFieldType::AUTO_COEFFICIENT);
      }
    }
  }

  if (config->GetDirectDiff()){
    for (unsigned short iField = 0; iField < historyOutput_List.size(); iField++){
      const string &fieldIdentifier = historyOutput_List[iField];
      const HistoryOutputField &currentField = historyOutput_Map.at(fieldIdentifier);
      if (currentField.fieldType == HistoryFieldType::COEFFICIENT){
        AddHistoryOutput("D_"      + fieldIdentifier, "d["     + currentField.fieldName + "]",
                         currentField.screenFormat, "D_"      + currentField.outputGroup,
                         "Derivative value (DIRECT_DIFF=YES)", HistoryFieldType::AUTO_COEFFICIENT);
      }
    }
  }

  if (config->GetTime_Domain() && config->GetDirectDiff()){
    for (unsigned short iField = 0; iField < historyOutput_List.size(); iField++){
      const string &fieldIdentifier = historyOutput_List[iField];
      const HistoryOutputField &currentField = historyOutput_Map.at(fieldIdentifier);
      if (currentField.fieldType == HistoryFieldType::COEFFICIENT){
        AddHistoryOutput("D_TAVG_" + fieldIdentifier, "dtavg[" + currentField.fieldName + "]",
                         currentField.screenFormat, "D_TAVG_" + currentField.outputGroup,
                         "Derivative of the time averaged value (DIRECT_DIFF=YES)", HistoryFieldType::AUTO_COEFFICIENT);
      }
    }
  }

  for (unsigned short iFieldConv = 0; iFieldConv < convFields.size(); iFieldConv++){
    const string &convField = convFields[iFieldConv];
    if (historyOutput_Map.count(convField) > 0){
      if (historyOutput_Map[convField].fieldType == HistoryFieldType::COEFFICIENT){
        AddHistoryOutput("CAUCHY_" + convField, "Cauchy["  + historyOutput_Map.at(convField).fieldName + "]",
                         ScreenOutputFormat::SCIENTIFIC, "CAUCHY", "Cauchy residual value of field set with CONV_FIELD.",
                         HistoryFieldType::AUTO_COEFFICIENT);
      }
    }
  }
  for (unsigned short iFieldConv = 0; iFieldConv < wndConvFields.size(); iFieldConv++){
    const string &wndConvField = wndConvFields[iFieldConv];
    if (historyOutput_Map.count(wndConvField) > 0){
      AddHistoryOutput("CAUCHY_" + wndConvField, "Cauchy[" + historyOutput_Map[wndConvField].fieldName  + "]", ScreenOutputFormat::SCIENTIFIC, "CAUCHY", "Cauchy residual value of field set with WND_CONV_FIELD.", HistoryFieldType::AUTO_COEFFICIENT);
    }
  }
}

bool COutput::WriteScreenHeader(const CConfig *config) {

  unsigned long RestartIter = 0;

  if (config->GetRestart() && config->GetTime_Domain()){
    RestartIter = config->GetRestart_Iter();
  }

  unsigned long ScreenWrt_Freq_Inner = config->GetScreen_Wrt_Freq(2);
  unsigned long ScreenWrt_Freq_Outer = config->GetScreen_Wrt_Freq(1);
  unsigned long ScreenWrt_Freq_Time  = config->GetScreen_Wrt_Freq(0);

  /*--- Header is always disabled for multizone problems unless explicitely requested --- */

  if (config->GetMultizone_Problem() && !config->GetWrt_ZoneConv()){
    return false;
  }

  /*--- Always print header if it is forced ---*/

  if (headerNeeded){
    headerNeeded = false;
    return true;
  }

  /* --- Always print header in the first iteration --- */

  if ((curInnerIter == 0) &&
      (curOuterIter == 0) &&
      (curTimeIter == RestartIter)){
    return true;
  }

  if (!PrintOutput(curTimeIter, ScreenWrt_Freq_Time)&&
      !(curTimeIter == config->GetnTime_Iter() - 1)){
    return false;
  }

  /*--- If there is no inner or outer iteration, don't print header ---*/
  if (ScreenWrt_Freq_Outer == 0 && ScreenWrt_Freq_Inner == 0){
    return false;
  }

  /*--- Print header if we are at the first inner iteration ---*/

  if (curInnerIter == 0){
    return true;
  }

  return false;
}

bool COutput::WriteScreenOutput(const CConfig *config) {

  unsigned long ScreenWrt_Freq_Inner = config->GetScreen_Wrt_Freq(2);
  unsigned long ScreenWrt_Freq_Outer = config->GetScreen_Wrt_Freq(1);
  unsigned long ScreenWrt_Freq_Time  = config->GetScreen_Wrt_Freq(0);

  if (config->GetMultizone_Problem() && !config->GetWrt_ZoneConv()){

    return false;

  }

  /*--- Check if screen output should be written --- */

  if (!PrintOutput(curTimeIter, ScreenWrt_Freq_Time)&&
      !(curTimeIter == config->GetnTime_Iter() - 1)){

    return false;

  }

  if (convergence) {return true;}

  if (!PrintOutput(curOuterIter, ScreenWrt_Freq_Outer) &&
      !(curOuterIter == config->GetnOuter_Iter() - 1)){

    return false;

  }

  if (!PrintOutput(curInnerIter, ScreenWrt_Freq_Inner) &&
      !(curInnerIter == config->GetnInner_Iter() - 1)){

    return false;

  }

  return true;

}

bool COutput::WriteHistoryFileOutput(const CConfig *config) {

  unsigned long HistoryWrt_Freq_Inner = config->GetHistory_Wrt_Freq(2);
  unsigned long HistoryWrt_Freq_Outer = config->GetHistory_Wrt_Freq(1);
  unsigned long HistoryWrt_Freq_Time  = config->GetHistory_Wrt_Freq(0);

  if (config->GetMultizone_Problem() && !config->GetWrt_ZoneHist()){

    return false;

  }

  /*--- Check if screen output should be written --- */

  if (!PrintOutput(curTimeIter, HistoryWrt_Freq_Time)&&
      !(curTimeIter == config->GetnTime_Iter() - 1)){

    return false;

  }

  if (convergence) {return true;}

  if (!PrintOutput(curOuterIter,HistoryWrt_Freq_Outer) &&
      !(curOuterIter == config->GetnOuter_Iter() - 1)){

    return false;

  }

  if (!PrintOutput(curInnerIter, HistoryWrt_Freq_Inner) &&
      !(curInnerIter == config->GetnInner_Iter() - 1)){

    return false;

  }

  return true;

}

bool COutput::WriteVolumeOutput(CConfig *config, unsigned long Iter, bool force_writing, unsigned short iFile){

  if (config->GetTime_Domain()){

    return ((Iter % config->GetVolumeOutputFrequency(iFile) == 0)) || force_writing;
  }
  else {
    return ((Iter > 0) && (Iter % config->GetVolumeOutputFrequency(iFile) == 0)) || force_writing;
  }
}

void COutput::SetCommonHistoryFields() {

  /// BEGIN_GROUP: ITERATION, DESCRIPTION: Iteration identifier.
  /// DESCRIPTION: The time iteration index.
  AddHistoryOutput("TIME_ITER", "Time_Iter", ScreenOutputFormat::INTEGER, "ITER", "Time iteration index");
  /// DESCRIPTION: The outer iteration index.
  AddHistoryOutput("OUTER_ITER", "Outer_Iter", ScreenOutputFormat::INTEGER, "ITER", "Outer iteration index");
  /// DESCRIPTION: The inner iteration index.
  AddHistoryOutput("INNER_ITER", "Inner_Iter", ScreenOutputFormat::INTEGER,  "ITER", "Inner iteration index");
  /// END_GROUP

  /// BEGIN_GROUP: TIME_DOMAIN, DESCRIPTION: Time integration information
  /// Description: The current time
  AddHistoryOutput("CUR_TIME", "Cur_Time", ScreenOutputFormat::SCIENTIFIC, "TIME_DOMAIN", "Current physical time (s)");
  /// Description: The current time step
  AddHistoryOutput("TIME_STEP", "Time_Step", ScreenOutputFormat::SCIENTIFIC, "TIME_DOMAIN", "Current time step (s)");

  /// DESCRIPTION: Currently used wall-clock time.
  AddHistoryOutput("WALL_TIME", "Time(sec)", ScreenOutputFormat::SCIENTIFIC, "WALL_TIME", "Average wall-clock time since the start of inner iterations.");

  AddHistoryOutput("NONPHYSICAL_POINTS", "Nonphysical_Points", ScreenOutputFormat::INTEGER, "NONPHYSICAL_POINTS", "The number of non-physical points in the solution");

}

void COutput::SetCustomOutputs(const CConfig* config) {

  const auto& inputString = config->GetCustomOutputs();
  if (inputString.empty()) return;

  /*--- Split the different functions. ---*/

  auto DebugPrint = [](const std::string& str) {
#ifndef NDEBUG
    std::cout << str << std::endl;
#endif
  };
  DebugPrint(inputString);

  const std::map<std::string, OperationType> opMap = {
    {"Macro", OperationType::MACRO},
    {"Function", OperationType::FUNCTION},
    {"AreaAvg", OperationType::AREA_AVG},
    {"AreaInt", OperationType::AREA_INT},
    {"MassFlowAvg", OperationType::MASSFLOW_AVG},
    {"MassFlowInt", OperationType::MASSFLOW_INT},
    {"Probe", OperationType::PROBE},
  };
  std::stringstream knownOps;
  for (const auto& item : opMap) knownOps << item.first << ", ";

  /*--- Split the input string into functions delimited by ";". ---*/
  std::vector<std::string> functions;

  const auto last = inputString.end();
  for (auto it = inputString.begin(); it != last;) {

    /*--- Find the start of the function name. ---*/
    while (it != last && (*it == ' ' || *it == ';')) ++it;
    if (it == last) break;

    /*--- Find the end of the function. ---*/
    const auto start = it;
    while (it != last && *it != ';') ++it;

    functions.emplace_back(start, it);
  }

  /*--- Process each function. ---*/
  size_t iFunc = 0;
  for (const auto& functionString : functions) {
    ++iFunc;
    DebugPrint(functionString);
    const auto last = functionString.end();
    for (auto it = functionString.begin(); it != last;) {

      /*--- Find the end of the function name. ---*/
      auto start = it;
      while (it != last && *it != ' ' && *it != ':') ++it;
      auto name = std::string(start, it);
      DebugPrint(name);

      /*--- Find the start and end of the operation type. ---*/
      while (it != last && (*it == ' ' || *it == ':')) ++it;
      start = it;
      while (it != last && *it != ' ' && *it != '{') ++it;
      const auto opType = std::string(start, it);
      DebugPrint(opType);

      auto item = opMap.find(opType);
      if (item == opMap.end()) {
        SU2_MPI::Error("Invalid operation type '" + opType + "', must be one of: " + knownOps.str(), CURRENT_FUNCTION);
      }
      const auto type = item->second;

      /*--- Find the user expression. ---*/
      while (it != last && (*it == ' ' || *it == '{')) ++it;
      start = it;
      while (it != last && *it != '}') ++it;
      auto func = std::string(start, it);
      DebugPrint(func);

      if (type == OperationType::MACRO) {
        /*--- Replace the expression in downstream functions, do not create a custom output for it. ---*/
        const auto key = '$' + name;
        for (auto i = iFunc; i < functions.size(); ++i) {
          size_t pos = 0;
          while ((pos = functions[i].find(key)) != std::string::npos) {
            functions[i].replace(pos, key.length(), func);
            DebugPrint(functions[i]);
          }
        }
        break;
      }

      customOutputs.push_back(CustomOutput());
      auto& output = customOutputs.back();

      output.name = std::move(name);
      output.type = type;
      output.func = std::move(func);
      output.expression = mel::Parse<passivedouble>(output.func, output.varSymbols);
#ifndef NDEBUG
      mel::Print(output.expression, output.varSymbols, std::cout);
#endif

      if (type == OperationType::FUNCTION) {
        AddHistoryOutput(output.name, output.name, ScreenOutputFormat::SCIENTIFIC, "CUSTOM", "Custom output");
        break;
      }

      /*--- Find the marker names. ---*/
      while (it != last && (*it == ' ' || *it == '}' || *it == '[')) ++it;
      while (it != last && *it != ']') {
        start = it;
        while (it != last && *it != ' ' && *it != ',' && *it != ']') ++it;
        output.markers.emplace_back(start, it);
        DebugPrint(output.markers.back());

        while (it != last && (*it == ' ' || *it == ',')) ++it;
      }
      /*--- Skip the terminating "]". ---*/
      if (it != last) ++it;

      AddHistoryOutput(output.name, output.name, ScreenOutputFormat::SCIENTIFIC, "CUSTOM", "Custom output");
    }
  }

}

void COutput::LoadCommonHistoryData(const CConfig *config) {

  SetHistoryOutputValue("TIME_STEP", config->GetDelta_UnstTimeND()*config->GetTime_Ref());

  /*--- Update the current time only if the time iteration has changed ---*/

  if (SU2_TYPE::Int(GetHistoryFieldValue("TIME_ITER")) != static_cast<int>(curTimeIter)) {
    SetHistoryOutputValue("CUR_TIME",  GetHistoryFieldValue("CUR_TIME") + GetHistoryFieldValue("TIME_STEP"));
  }

  SetHistoryOutputValue("TIME_ITER",  curTimeIter);
  SetHistoryOutputValue("INNER_ITER", curInnerIter);
  SetHistoryOutputValue("OUTER_ITER", curOuterIter);

  su2double StopTime, UsedTime;

  StopTime = SU2_MPI::Wtime();

  UsedTime = (StopTime - config->Get_StartTime())/(curInnerIter+1);

  SetHistoryOutputValue("WALL_TIME", UsedTime);

  SetHistoryOutputValue("NONPHYSICAL_POINTS", config->GetNonphysical_Points());
}


void COutput::PrintHistoryFields() const {

  if (rank != MASTER_NODE) return;

  PrintingToolbox::CTablePrinter HistoryFieldTable(&std::cout);

  size_t NameSize = 0, GroupSize = 0, DescrSize = 0;

  for (int perSurf = 0; perSurf < 2; ++perSurf) {
    const auto& outputList = perSurf ? historyOutputPerSurface_List : historyOutput_List;

    for (const auto& outputName : outputList) {
      const HistoryOutputField* Field = nullptr;
      if (!perSurf) {
        Field = &historyOutput_Map.at(outputName);
      } else {
        Field = &historyOutputPerSurface_Map.at(outputName)[0];
      }
      if (perSurf || !Field->description.empty()) {
        NameSize = std::max(NameSize, outputName.size());
        GroupSize = std::max(GroupSize, Field->outputGroup.size());
        DescrSize = std::max(DescrSize, Field->description.size());
      }
    }
  }

  cout << "Available screen/history output fields for the current configuration in " << multiZoneHeaderString << ":\n";

  HistoryFieldTable.AddColumn("Name", NameSize);
  HistoryFieldTable.AddColumn("Group Name", GroupSize);
  HistoryFieldTable.AddColumn("Type",5);
  HistoryFieldTable.AddColumn("Description", DescrSize);
  HistoryFieldTable.SetAlign(PrintingToolbox::CTablePrinter::LEFT);

  HistoryFieldTable.PrintHeader();
  string type;

  for (int perSurf = 0; perSurf < 2; ++perSurf) {
    const auto& outputList = perSurf ? historyOutputPerSurface_List : historyOutput_List;

    for (const auto& outputName : outputList) {
      const HistoryOutputField* Field = nullptr;
      if (!perSurf) {
        Field = &historyOutput_Map.at(outputName);
      } else {
        Field = &historyOutputPerSurface_Map.at(outputName)[0];
      }

      if (!perSurf && Field->description.empty()) continue;

      if (Field->fieldType == HistoryFieldType::DEFAULT ||
          Field->fieldType == HistoryFieldType::COEFFICIENT ||
          Field->fieldType == HistoryFieldType::RESIDUAL) {
        switch (Field->fieldType) {
          case HistoryFieldType::COEFFICIENT:
            type = "C";
            break;
          case HistoryFieldType::RESIDUAL:
            type = "R";
            break;
          default:
            type = "D";
            break;
        }
        HistoryFieldTable << outputName << Field->outputGroup << type << Field->description;
      }
    }
  }

  HistoryFieldTable.PrintFooter();

  cout << "Type legend: Default (D), Residual (R), Coefficient (C)\n";
  cout << "Generated screen/history fields (only first field of every group is shown):\n";

  PrintingToolbox::CTablePrinter ModifierTable(&std::cout);

  ModifierTable.AddColumn("Name", NameSize);
  ModifierTable.AddColumn("Group Name", GroupSize);
  ModifierTable.AddColumn("Type",5);
  ModifierTable.AddColumn("Description", DescrSize);
  ModifierTable.SetAlign(PrintingToolbox::CTablePrinter::LEFT);
  ModifierTable.PrintHeader();

  std::map<string, bool> GroupVisited;

  for (unsigned short iField = 0; iField < historyOutput_List.size(); iField++){

    const auto& Field = historyOutput_Map.at(historyOutput_List[iField]);

    if ((Field.fieldType == HistoryFieldType::AUTO_COEFFICIENT ||
         Field.fieldType == HistoryFieldType::AUTO_RESIDUAL) &&
        (GroupVisited.count(Field.outputGroup) == 0)){
      switch (Field.fieldType) {
        case HistoryFieldType::AUTO_COEFFICIENT:
          type = "AC";
          break;
        case HistoryFieldType::AUTO_RESIDUAL:
          type = "AR";
          break;
        default:
          type = "AD";
          break;
      }

      if (Field.description != "")
        ModifierTable << historyOutput_List[iField] << Field.outputGroup << type << Field.description;

      GroupVisited[Field.outputGroup] = true;
    }
  }

  ModifierTable.PrintFooter();

}

void COutput::PrintVolumeFields(){

  if (rank == MASTER_NODE){

    PrintingToolbox::CTablePrinter VolumeFieldTable(&std::cout);

    unsigned short NameSize = 0, GroupSize = 0, DescrSize = 0;

    for (unsigned short iField = 0; iField < volumeOutput_List.size(); iField++){

      VolumeOutputField &Field = volumeOutput_Map.at(volumeOutput_List[iField]);

      if (Field.description != ""){
        if (volumeOutput_List[iField].size() > NameSize){
          NameSize = volumeOutput_List[iField].size();
        }
        if (Field.outputGroup.size() > GroupSize){
          GroupSize = Field.outputGroup.size();
        }
        if (Field.description.size() > DescrSize){
          DescrSize = Field.description.size();
        }
      }
    }

    cout << "Available volume output fields for the current configuration in " << multiZoneHeaderString << ":" << endl;
    cout << "Note: COORDINATES and SOLUTION groups are always in the volume output." << endl;
    VolumeFieldTable.AddColumn("Name", NameSize);
    VolumeFieldTable.AddColumn("Group Name", GroupSize);
    VolumeFieldTable.AddColumn("Description", DescrSize);
    VolumeFieldTable.SetAlign(PrintingToolbox::CTablePrinter::LEFT);

    VolumeFieldTable.PrintHeader();

    for (unsigned short iField = 0; iField < volumeOutput_List.size(); iField++){

      VolumeOutputField &Field = volumeOutput_Map.at(volumeOutput_List[iField]);

      if (Field.description != "")
        VolumeFieldTable << volumeOutput_List[iField] << Field.outputGroup << Field.description;

    }

    VolumeFieldTable.PrintFooter();
  }
}

void COutput::InitTurboPerformance(const CConfig *config){
  unsigned short iDim, iSpan, iMarker;

  /*--- Initializate quantities for turboperformace ---*/
  nSpanWiseSections = config->GetnSpanMaxAllZones();
  nMarkerTurboPerf  = config->GetnMarker_TurboPerformance();


  TotalStaticEfficiency         = new su2double*[nMarkerTurboPerf];
  TotalTotalEfficiency          = new su2double*[nMarkerTurboPerf];
  KineticEnergyLoss             = new su2double*[nMarkerTurboPerf];
  TRadius                       = new su2double*[nMarkerTurboPerf];
  TotalPressureLoss             = new su2double*[nMarkerTurboPerf];
  MassFlowIn                    = new su2double*[nMarkerTurboPerf];
  MassFlowOut                   = new su2double*[nMarkerTurboPerf];
  FlowAngleIn                   = new su2double*[nMarkerTurboPerf];
  FlowAngleIn_BC                = new su2double*[nMarkerTurboPerf];
  FlowAngleOut                  = new su2double*[nMarkerTurboPerf];
  EulerianWork                  = new su2double*[nMarkerTurboPerf];
  TotalEnthalpyIn               = new su2double*[nMarkerTurboPerf];
  TotalEnthalpyIn_BC            = new su2double*[nMarkerTurboPerf];
  EntropyIn                     = new su2double*[nMarkerTurboPerf];
  EntropyOut                    = new su2double*[nMarkerTurboPerf];
  EntropyIn_BC                  = new su2double*[nMarkerTurboPerf];
  PressureRatio                 = new su2double*[nMarkerTurboPerf];
  TotalTemperatureIn            = new su2double*[nMarkerTurboPerf];
  EnthalpyOut                   = new su2double*[nMarkerTurboPerf];
  MachIn                        = new su2double**[nMarkerTurboPerf];
  MachOut                       = new su2double**[nMarkerTurboPerf];
  VelocityOutIs                 = new su2double*[nMarkerTurboPerf];
  DensityIn                     = new su2double*[nMarkerTurboPerf];
  PressureIn                    = new su2double*[nMarkerTurboPerf];
  TurboVelocityIn               = new su2double**[nMarkerTurboPerf];
  DensityOut                    = new su2double*[nMarkerTurboPerf];
  PressureOut                   = new su2double*[nMarkerTurboPerf];
  TurboVelocityOut              = new su2double**[nMarkerTurboPerf];
  EnthalpyOutIs                 = new su2double*[nMarkerTurboPerf];
  EntropyGen                    = new su2double*[nMarkerTurboPerf];
  AbsFlowAngleIn                = new su2double*[nMarkerTurboPerf];
  TotalEnthalpyOut              = new su2double*[nMarkerTurboPerf];
  TotalEnthalpyOutIs            = new su2double*[nMarkerTurboPerf];
  RothalpyIn                    = new su2double*[nMarkerTurboPerf];
  RothalpyOut                   = new su2double*[nMarkerTurboPerf];
  AbsFlowAngleOut               = new su2double*[nMarkerTurboPerf];
  PressureOut_BC                = new su2double*[nMarkerTurboPerf];
  TemperatureIn                 = new su2double*[nMarkerTurboPerf];
  TemperatureOut                = new su2double*[nMarkerTurboPerf];
  TotalPressureIn               = new su2double*[nMarkerTurboPerf];
  TotalPressureOut              = new su2double*[nMarkerTurboPerf];
  TotalTemperatureOut           = new su2double*[nMarkerTurboPerf];
  EnthalpyIn                    = new su2double*[nMarkerTurboPerf];
  TurbIntensityIn               = new su2double*[nMarkerTurboPerf];
  Turb2LamViscRatioIn           = new su2double*[nMarkerTurboPerf];
  TurbIntensityOut              = new su2double*[nMarkerTurboPerf];
  Turb2LamViscRatioOut          = new su2double*[nMarkerTurboPerf];
  NuFactorIn                    = new su2double*[nMarkerTurboPerf];
  NuFactorOut                   = new su2double*[nMarkerTurboPerf];

  for (iMarker = 0; iMarker < nMarkerTurboPerf; iMarker++){
    TotalStaticEfficiency   [iMarker] = new su2double [nSpanWiseSections + 1];
    TotalTotalEfficiency    [iMarker] = new su2double [nSpanWiseSections + 1];
    KineticEnergyLoss       [iMarker] = new su2double [nSpanWiseSections + 1];
    TRadius                 [iMarker] = new su2double [nSpanWiseSections + 1];
    TotalPressureLoss       [iMarker] = new su2double [nSpanWiseSections + 1];
    MassFlowIn              [iMarker] = new su2double [nSpanWiseSections + 1];
    MassFlowOut             [iMarker] = new su2double [nSpanWiseSections + 1];
    FlowAngleIn             [iMarker] = new su2double [nSpanWiseSections + 1];
    FlowAngleIn_BC          [iMarker] = new su2double [nSpanWiseSections + 1];
    FlowAngleOut            [iMarker] = new su2double [nSpanWiseSections + 1];
    EulerianWork            [iMarker] = new su2double [nSpanWiseSections + 1];
    TotalEnthalpyIn         [iMarker] = new su2double [nSpanWiseSections + 1];
    TotalEnthalpyIn_BC      [iMarker] = new su2double [nSpanWiseSections + 1];
    EntropyIn               [iMarker] = new su2double [nSpanWiseSections + 1];
    EntropyOut              [iMarker] = new su2double [nSpanWiseSections + 1];
    EntropyIn_BC            [iMarker] = new su2double [nSpanWiseSections + 1];
    PressureRatio           [iMarker] = new su2double [nSpanWiseSections + 1];
    TotalTemperatureIn      [iMarker] = new su2double [nSpanWiseSections + 1];
    EnthalpyOut             [iMarker] = new su2double [nSpanWiseSections + 1];
    MachIn                  [iMarker] = new su2double*[nSpanWiseSections + 1];
    MachOut                 [iMarker] = new su2double*[nSpanWiseSections + 1];
    VelocityOutIs           [iMarker] = new su2double [nSpanWiseSections + 1];
    DensityIn               [iMarker] = new su2double [nSpanWiseSections + 1];
    PressureIn              [iMarker] = new su2double [nSpanWiseSections + 1];
    TurboVelocityIn         [iMarker] = new su2double*[nSpanWiseSections + 1];
    DensityOut              [iMarker] = new su2double [nSpanWiseSections + 1];
    PressureOut             [iMarker] = new su2double [nSpanWiseSections + 1];
    TurboVelocityOut        [iMarker] = new su2double*[nSpanWiseSections + 1];
    EnthalpyOutIs           [iMarker] = new su2double [nSpanWiseSections + 1];
    EntropyGen              [iMarker] = new su2double [nSpanWiseSections + 1];
    AbsFlowAngleIn          [iMarker] = new su2double [nSpanWiseSections + 1];
    TotalEnthalpyOut        [iMarker] = new su2double [nSpanWiseSections + 1];
    TotalEnthalpyOutIs      [iMarker] = new su2double [nSpanWiseSections + 1];
    RothalpyIn              [iMarker] = new su2double [nSpanWiseSections + 1];
    RothalpyOut             [iMarker] = new su2double [nSpanWiseSections + 1];
    AbsFlowAngleOut         [iMarker] = new su2double [nSpanWiseSections + 1];
    PressureOut_BC          [iMarker] = new su2double [nSpanWiseSections + 1];
    TemperatureIn           [iMarker] = new su2double [nSpanWiseSections + 1];
    TemperatureOut          [iMarker] = new su2double [nSpanWiseSections + 1];
    TotalPressureIn         [iMarker] = new su2double [nSpanWiseSections + 1];
    TotalPressureOut        [iMarker] = new su2double [nSpanWiseSections + 1];
    TotalTemperatureOut     [iMarker] = new su2double [nSpanWiseSections + 1];
    EnthalpyIn              [iMarker] = new su2double [nSpanWiseSections + 1];
    TurbIntensityIn         [iMarker] = new su2double [nSpanWiseSections + 1];
    Turb2LamViscRatioIn     [iMarker] = new su2double [nSpanWiseSections + 1];
    TurbIntensityOut        [iMarker] = new su2double [nSpanWiseSections + 1];
    Turb2LamViscRatioOut    [iMarker] = new su2double [nSpanWiseSections + 1];
    NuFactorIn              [iMarker] = new su2double [nSpanWiseSections + 1];
    NuFactorOut             [iMarker] = new su2double [nSpanWiseSections + 1];


    for (iSpan = 0; iSpan < nSpanWiseSections + 1; iSpan++){
      TotalStaticEfficiency   [iMarker][iSpan] = 0.0;
      TotalTotalEfficiency    [iMarker][iSpan] = 0.0;
      KineticEnergyLoss       [iMarker][iSpan] = 0.0;
      TRadius                 [iMarker][iSpan] = 0.0;
      TotalPressureLoss       [iMarker][iSpan] = 0.0;
      MassFlowIn              [iMarker][iSpan] = 0.0;
      MassFlowOut             [iMarker][iSpan] = 0.0;
      FlowAngleIn             [iMarker][iSpan] = 0.0;
      FlowAngleIn_BC          [iMarker][iSpan] = config->GetFlowAngleIn_BC();
      FlowAngleOut            [iMarker][iSpan] = 0.0;
      EulerianWork            [iMarker][iSpan] = 0.0;
      TotalEnthalpyIn         [iMarker][iSpan] = 0.0;
      TotalEnthalpyIn_BC      [iMarker][iSpan] = 0.0;
      EntropyIn               [iMarker][iSpan] = 0.0;
      EntropyOut              [iMarker][iSpan] = 0.0;
      EntropyIn_BC            [iMarker][iSpan] = 0.0;
      PressureRatio           [iMarker][iSpan] = 0.0;
      TotalTemperatureIn      [iMarker][iSpan] = 0.0;
      EnthalpyOut             [iMarker][iSpan] = 0.0;


      VelocityOutIs           [iMarker][iSpan] = 0.0;
      DensityIn               [iMarker][iSpan] = 0.0;
      PressureIn              [iMarker][iSpan] = 0.0;

      DensityOut              [iMarker][iSpan] = 0.0;
      PressureOut             [iMarker][iSpan] = 0.0;

      EnthalpyOutIs           [iMarker][iSpan] = 0.0;
      EntropyGen              [iMarker][iSpan] = 0.0;
      AbsFlowAngleIn          [iMarker][iSpan] = 0.0;
      TotalEnthalpyOut        [iMarker][iSpan] = 0.0;
      TotalEnthalpyOutIs      [iMarker][iSpan] = 0.0;
      RothalpyIn              [iMarker][iSpan] = 0.0;
      RothalpyOut             [iMarker][iSpan] = 0.0;
      AbsFlowAngleOut         [iMarker][iSpan] = 0.0;
      PressureOut_BC          [iMarker][iSpan] = config->GetPressureOut_BC();

      TemperatureIn           [iMarker][iSpan] = 0.0;
      TemperatureOut          [iMarker][iSpan] = 0.0;
      TotalPressureIn         [iMarker][iSpan] = 0.0;
      TotalPressureOut        [iMarker][iSpan] = 0.0;
      TotalTemperatureOut     [iMarker][iSpan] = 0.0;
      EnthalpyIn              [iMarker][iSpan] = 0.0;
      TurbIntensityIn         [iMarker][iSpan] = 0.0;
      Turb2LamViscRatioIn     [iMarker][iSpan] = 0.0;
      TurbIntensityOut        [iMarker][iSpan] = 0.0;
      Turb2LamViscRatioOut    [iMarker][iSpan] = 0.0;
      NuFactorIn              [iMarker][iSpan] = 0.0;
      NuFactorOut             [iMarker][iSpan] = 0.0;
      MachIn                  [iMarker][iSpan] = new su2double[4];
      MachOut                 [iMarker][iSpan] = new su2double[4];
      TurboVelocityIn         [iMarker][iSpan] = new su2double[4];
      TurboVelocityOut        [iMarker][iSpan] = new su2double[4];

      for (iDim = 0; iDim < 4; iDim++){
        MachIn           [iMarker][iSpan][iDim]   = 0.0;
        MachOut          [iMarker][iSpan][iDim]   = 0.0;
        TurboVelocityIn  [iMarker][iSpan][iDim]   = 0.0;
        TurboVelocityOut [iMarker][iSpan][iDim]   = 0.0;
      }
    }
  }
}

void COutput::ComputeTurboPerformance(CConfig *config, CGeometry *geometry, CSolver *solver){
  CFluidModel *FluidModel;
  unsigned short nDim = geometry->GetnDim();
  unsigned short iMarkerTP, iSpan, iDim, iStage, iBlade;
  unsigned short nMarkerTP = config->GetnMarker_Turbomachinery();
  FluidModel = solver->GetFluidModel();
  su2double area, absVel2, soundSpeed, mach, tangVel, tangVel2, *relVel, relVel2;
  su2double relPressureIn, relPressureOut, enthalpyOutIs, relVelOutIs2;
  relVel = new su2double[nDim];
  su2double muLam, kine, omega, nu;
  bool turbulent = ((config->GetKind_Solver() == MAIN_SOLVER::RANS) || (config->GetKind_Solver() == MAIN_SOLVER::DISC_ADJ_RANS));
  bool menter_sst       = (config->GetKind_Turb_Model() == TURB_MODEL::SST);

  unsigned short nBladesRow, nStages;

  nBladesRow = config->GetnMarker_Turbomachinery();
  nStages    = SU2_TYPE::Int(nBladesRow/2);


  /*--- Compute BC imposed value for convergence monitoring ---*/
  for(iMarkerTP = 0; iMarkerTP < nMarkerTP; iMarkerTP++ ){
    for(iSpan = 0; iSpan < config->GetnSpan_iZones(iMarkerTP) + 1; iSpan++){
      if(config->GetRampOutletPressure() && config->GetInnerIter() > 0){
        PressureOut_BC[iMarkerTP][iSpan] = config->GetMonitorOutletPressure()/config->GetPressure_Ref();
      }
      FluidModel->SetTDState_PT(config->GetTotalPressureIn_BC(), config->GetTotalTemperatureIn_BC());
      TotalEnthalpyIn_BC[iMarkerTP][iSpan] = FluidModel->GetStaticEnergy()+ FluidModel->GetPressure()/FluidModel->GetDensity();
      EntropyIn_BC[iMarkerTP][iSpan]       = FluidModel->GetEntropy();
    }
  }

  /*--- Compute performance for each blade ---*/
  for(iMarkerTP = 0; iMarkerTP < nMarkerTP; iMarkerTP++ ){
    for(iSpan = 0; iSpan < config->GetnSpan_iZones(iMarkerTP) + 1; iSpan++){


      /*--- INFLOW ---*/
      /*--- Retrieve Inflow primitive quantities ---*/
      DensityIn[iMarkerTP][iSpan]          = solver->GetDensityIn(iMarkerTP, iSpan);
      PressureIn[iMarkerTP][iSpan]         = solver->GetPressureIn(iMarkerTP, iSpan);

      absVel2 = 0.0;

      for (iDim = 0; iDim < nDim; iDim++){
        TurboVelocityIn[iMarkerTP][iSpan][iDim]    = solver->GetTurboVelocityIn(iMarkerTP, iSpan)[iDim];
        absVel2   += TurboVelocityIn[iMarkerTP][iSpan][iDim]*TurboVelocityIn[iMarkerTP][iSpan][iDim];
      }
      TurboVelocityIn[iMarkerTP][iSpan][nDim] = sqrt(absVel2);

      TRadius[iMarkerTP][iSpan]  = geometry->GetTurboRadiusIn(iMarkerTP, iSpan);
      area                       = geometry->GetSpanAreaIn(iMarkerTP, iSpan);

      /*--- Compute static Inflow quantities ---*/
      FluidModel->SetTDState_Prho(PressureIn[iMarkerTP][iSpan], DensityIn[iMarkerTP][iSpan]);
      EntropyIn[iMarkerTP][iSpan]          = FluidModel->GetEntropy();
      MassFlowIn[iMarkerTP][iSpan]         = config->GetnBlades(iMarkerTP)*DensityIn[iMarkerTP][iSpan]*TurboVelocityIn[iMarkerTP][iSpan][0]*area;
      AbsFlowAngleIn[iMarkerTP][iSpan]     = atan(TurboVelocityIn[iMarkerTP][iSpan][1]/TurboVelocityIn[iMarkerTP][iSpan][0]);
      EnthalpyIn[iMarkerTP][iSpan]         = FluidModel->GetStaticEnergy() + PressureIn[iMarkerTP][iSpan]/DensityIn[iMarkerTP][iSpan];
      soundSpeed                           = FluidModel->GetSoundSpeed();


      /*--- Compute Total Inflow quantities ---*/
      TotalEnthalpyIn[iMarkerTP][iSpan]    = EnthalpyIn[iMarkerTP][iSpan] + 0.5*absVel2;
      FluidModel->SetTDState_hs(TotalEnthalpyIn[iMarkerTP][iSpan], EntropyIn[iMarkerTP][iSpan]);
      TotalPressureIn[iMarkerTP][iSpan]    = FluidModel->GetPressure();
      TotalTemperatureIn[iMarkerTP][iSpan] = FluidModel->GetTemperature();

      /*--- Retrieve Inflow relative quantities ---*/
      tangVel = geometry->GetTangGridVelIn(iMarkerTP, iSpan);
      tangVel2 = tangVel*tangVel;

      for (iDim = 0; iDim < nDim; iDim++){
        relVel[iDim] = TurboVelocityIn[iMarkerTP][iSpan][iDim];
      }
      relVel[1] -= tangVel;

      relVel2 = 0.0;
      for (iDim = 0; iDim < nDim; iDim++){
        relVel2 += relVel[iDim]*relVel[iDim];
      }

      /*--- Compute Total relative Inflow quantities ---*/
      RothalpyIn[iMarkerTP][iSpan]  = EnthalpyIn[iMarkerTP][iSpan] + 0.5*relVel2 - 0.5*tangVel2;
      FluidModel->SetTDState_hs(RothalpyIn[iMarkerTP][iSpan], EntropyIn[iMarkerTP][iSpan]);
      relPressureIn   = FluidModel->GetPressure();

      /*--- Compute kinematic relative Inflow quantities ---*/
      FlowAngleIn[iMarkerTP][iSpan]    = atan(relVel[1]/relVel[0]);
      mach          = 0.0;
      for (iDim = 0; iDim < nDim; iDim++){
        MachIn[iMarkerTP][iSpan][iDim] = relVel[iDim]/soundSpeed;
        mach = MachIn[iMarkerTP][iSpan][iDim]*MachIn[iMarkerTP][iSpan][iDim];
      }
      MachIn[iMarkerTP][iSpan][nDim]   = sqrt(mach);

      /*--- Compute Turbulent Inflow quantities ---*/
      if(turbulent){
        FluidModel->SetTDState_Prho(PressureIn[iMarkerTP][iSpan], DensityIn[iMarkerTP][iSpan]);
        muLam  = FluidModel->GetLaminarViscosity();
        if(menter_sst){
          kine   = solver->GetKineIn(iMarkerTP, iSpan);
          omega  = solver->GetOmegaIn(iMarkerTP, iSpan);
          TurbIntensityIn[iMarkerTP][iSpan]     =  sqrt(2.0/3.0*kine/absVel2);
          Turb2LamViscRatioIn[iMarkerTP][iSpan] = DensityIn[iMarkerTP][iSpan]*kine/(muLam*omega);
//          TurbIntensityIn[iMarkerTP][iSpan]     =  kine;
//          Turb2LamViscRatioIn[iMarkerTP][iSpan] = omega;
        }
        else{
          nu = solver->GetNuIn(iMarkerTP, iSpan);
          NuFactorIn[iMarkerTP][iSpan]          = nu*DensityIn[iMarkerTP][iSpan]/muLam;
        }
      }

      /*--- OUTFLOW ---*/
      /*--- Retrieve Outflow primitive quantities ---*/
      DensityOut[iMarkerTP][iSpan]         = solver->GetDensityOut(iMarkerTP, iSpan);
      PressureOut[iMarkerTP][iSpan]        = solver->GetPressureOut(iMarkerTP, iSpan);
      absVel2 = 0.0;

      for (iDim = 0; iDim < nDim; iDim++){
        TurboVelocityOut[iMarkerTP][iSpan][iDim]    = solver->GetTurboVelocityOut(iMarkerTP, iSpan)[iDim];
        absVel2   += TurboVelocityOut[iMarkerTP][iSpan][iDim]*TurboVelocityOut[iMarkerTP][iSpan][iDim];
      }
      TurboVelocityOut[iMarkerTP][iSpan][nDim] = sqrt(absVel2);


      for (iDim = 0; iDim < 3; iDim++){
      }
      area   = geometry->GetSpanAreaOut(iMarkerTP, iSpan);


      /*--- Compute all the Outflow quantities ---*/
      FluidModel->SetTDState_Prho(PressureOut[iMarkerTP][iSpan], DensityOut[iMarkerTP][iSpan]);
      EntropyOut[iMarkerTP][iSpan]          = FluidModel->GetEntropy();
      MassFlowOut[iMarkerTP][iSpan]         = config->GetnBlades(iMarkerTP)*DensityOut[iMarkerTP][iSpan]*TurboVelocityOut[iMarkerTP][iSpan][0]*area;
      AbsFlowAngleOut[iMarkerTP][iSpan]     = atan(TurboVelocityOut[iMarkerTP][iSpan][1]/TurboVelocityOut[iMarkerTP][iSpan][0]);
      EnthalpyOut[iMarkerTP][iSpan]         = FluidModel->GetStaticEnergy() + PressureOut[iMarkerTP][iSpan]/DensityOut[iMarkerTP][iSpan];
      soundSpeed                            = FluidModel->GetSoundSpeed();

      /*--- Compute Total Outflow quantities ---*/
      TotalEnthalpyOut[iMarkerTP][iSpan]    = EnthalpyOut[iMarkerTP][iSpan] + 0.5*absVel2;
      FluidModel->SetTDState_hs(TotalEnthalpyOut[iMarkerTP][iSpan], EntropyOut[iMarkerTP][iSpan]);
      TotalPressureOut[iMarkerTP][iSpan]    = FluidModel->GetPressure();
      TotalTemperatureOut[iMarkerTP][iSpan] = FluidModel->GetTemperature();

      /*--- Retrieve relative Outflow  quantities ---*/
      tangVel  = geometry->GetTangGridVelOut(iMarkerTP, iSpan);
      tangVel2 = tangVel*tangVel;

      for (iDim = 0; iDim < nDim; iDim++){
        relVel[iDim] = TurboVelocityOut[iMarkerTP][iSpan][iDim];
      }
      relVel[1] -= tangVel;

      relVel2 = 0.0;
      for (iDim = 0; iDim < nDim; iDim++){
        relVel2 += relVel[iDim]*relVel[iDim];
      }

      /*--- Compute Total relative Outflow quantities ---*/
      RothalpyOut[iMarkerTP][iSpan] = EnthalpyOut[iMarkerTP][iSpan] + 0.5*relVel2 - 0.5*tangVel2;
      FluidModel->SetTDState_hs(RothalpyOut[iMarkerTP][iSpan], EntropyOut[iMarkerTP][iSpan]);
      relPressureOut  = FluidModel->GetPressure();

      /*--- Compute isentropic Outflow quantities ---*/
      FluidModel->SetTDState_Ps(PressureOut[iMarkerTP][iSpan], EntropyIn[iMarkerTP][iSpan]);
      enthalpyOutIs   = FluidModel->GetStaticEnergy() + PressureOut[iMarkerTP][iSpan]/FluidModel->GetDensity();
      relVelOutIs2    = 2*(RothalpyOut[iMarkerTP][iSpan] - enthalpyOutIs) + tangVel2;


      /*--- Compute kinematic relative Outflow quantities ---*/
      FlowAngleOut[iMarkerTP][iSpan] = atan(relVel[1]/relVel[0]);
      mach   = 0.0;
      for (iDim = 0; iDim < nDim; iDim++){
        MachOut[iMarkerTP][iSpan][iDim] = relVel[iDim]/soundSpeed;
        mach = MachOut[iMarkerTP][iSpan][iDim]*MachOut[iMarkerTP][iSpan][iDim];
      }
      MachOut[iMarkerTP][iSpan][nDim]   = sqrt(mach);

      /*--- Compute Turbulent Outflow quantities ---*/
      if(turbulent){
        FluidModel->SetTDState_Prho(PressureOut[iMarkerTP][iSpan], DensityOut[iMarkerTP][iSpan]);
        muLam  = FluidModel->GetLaminarViscosity();
        if(menter_sst){
          kine   = solver->GetKineOut(iMarkerTP, iSpan);
          omega  = solver->GetOmegaOut(iMarkerTP, iSpan);
          TurbIntensityOut[iMarkerTP][iSpan]     =  sqrt(2.0/3.0*kine/absVel2);
          Turb2LamViscRatioOut[iMarkerTP][iSpan] = DensityOut[iMarkerTP][iSpan]*kine/(muLam*omega);
//          TurbIntensityOut[iMarkerTP][iSpan]     =  kine;
//          Turb2LamViscRatioOut[iMarkerTP][iSpan] = omega;
        }
        else{
          nu = solver->GetNuOut(iMarkerTP, iSpan);
          NuFactorOut[iMarkerTP][iSpan]          = nu*DensityOut[iMarkerTP][iSpan]/muLam;
        }
      }

      /*--- TURBO-PERFORMANCE---*/
      EntropyGen[iMarkerTP][iSpan]         = (EntropyOut[iMarkerTP][iSpan] - EntropyIn[iMarkerTP][iSpan])/abs(EntropyIn_BC[iMarkerTP][iSpan] + 1);
      EulerianWork[iMarkerTP][iSpan]       = TotalEnthalpyIn[iMarkerTP][iSpan] - TotalEnthalpyOut[iMarkerTP][iSpan];
      TotalPressureLoss[iMarkerTP][iSpan]  = (relPressureIn - relPressureOut)/(relPressureIn - PressureOut[iMarkerTP][iSpan]);
      KineticEnergyLoss[iMarkerTP][iSpan]  = 2*(EnthalpyOut[iMarkerTP][iSpan] - enthalpyOutIs)/relVelOutIs2;
      PressureRatio[iMarkerTP][iSpan]      = TotalPressureOut[iMarkerTP][iSpan]/TotalPressureIn[iMarkerTP][iSpan];
      EnthalpyOutIs[iMarkerTP][iSpan]      = (pow(TotalPressureOut[iMarkerTP][iSpan]/TotalPressureIn[iMarkerTP][iSpan], 0.4/1.4) - 1.0)/(TotalTemperatureOut[iMarkerTP][iSpan]/TotalTemperatureIn[iMarkerTP][iSpan] -1.0);
    }
  }

  if(nBladesRow > 1){
    /*--- Compute performance for each stage ---*/

    EulerianWork[nBladesRow + nStages][nSpanWiseSections]           = 0.0;
    /*---Comnpute performance for each stage---*/
    for(iStage = 0; iStage < nStages; iStage++ ){
      FluidModel->SetTDState_Ps(PressureOut[iStage*2 +1][config->GetnSpan_iZones(iStage*2 +1)], EntropyIn[iStage*2][config->GetnSpan_iZones(iStage*2)]);
      EnthalpyOutIs[nBladesRow + iStage][nSpanWiseSections]         = FluidModel->GetStaticEnergy() + PressureOut[iStage*2 +1][config->GetnSpan_iZones(iStage*2 +1)]/FluidModel->GetDensity();
      FluidModel->SetTDState_Prho(PressureOut[iStage*2 +1][config->GetnSpan_iZones(iStage*2 +1)], DensityOut[iStage*2 +1][config->GetnSpan_iZones(iStage*2 +1)]);
      absVel2 = 0.0;
      for (iDim = 0; iDim<nDim; iDim++)
        absVel2 += TurboVelocityOut[iStage*2 +1][config->GetnSpan_iZones(iStage*2 +1)][iDim]*TurboVelocityOut[iStage*2 +1][config->GetnSpan_iZones(iStage*2 +1)][iDim];
      TotalEnthalpyOutIs[nBladesRow + iStage][nSpanWiseSections]    = EnthalpyOutIs[nBladesRow + iStage][nSpanWiseSections] + 0.5*absVel2;

      TotalTotalEfficiency[nBladesRow + iStage][nSpanWiseSections]  = (TotalEnthalpyIn[iStage*2][config->GetnSpan_iZones(iStage*2)] - TotalEnthalpyOut[iStage*2 + 1][config->GetnSpan_iZones(iStage*2 +1)]);
      TotalTotalEfficiency[nBladesRow + iStage][nSpanWiseSections]  /= (TotalEnthalpyIn[iStage*2][config->GetnSpan_iZones(iStage*2)] - TotalEnthalpyOutIs[nBladesRow + iStage][nSpanWiseSections]);
      TotalStaticEfficiency[nBladesRow + iStage][nSpanWiseSections] = (TotalEnthalpyIn[iStage*2][config->GetnSpan_iZones(iStage*2)] - TotalEnthalpyOut[iStage*2 + 1][config->GetnSpan_iZones(iStage*2+1)]);
      TotalStaticEfficiency[nBladesRow + iStage][nSpanWiseSections] /= (TotalEnthalpyIn[iStage*2][config->GetnSpan_iZones(iStage*2)] - EnthalpyOutIs[nBladesRow + iStage][nSpanWiseSections]);
      PressureRatio[nBladesRow + iStage][nSpanWiseSections]         = (PressureRatio[iStage*2][config->GetnSpan_iZones(iStage*2)]*PressureOut[iStage*2][config->GetnSpan_iZones(iStage*2)]/PressureOut[iStage*2 + 1][config->GetnSpan_iZones(iStage*2+1)]);
      MassFlowIn[nBladesRow + iStage][nSpanWiseSections]            = MassFlowIn[iStage*2][config->GetnSpan_iZones(iStage*2)];
      MassFlowOut[nBladesRow + iStage][nSpanWiseSections]           = MassFlowOut[iStage*2 + 1][config->GetnSpan_iZones(iStage*2+1)];
      EntropyGen[nBladesRow + iStage][nSpanWiseSections]            = EntropyGen[iStage*2 + 1][config->GetnSpan_iZones(iStage*2 +1)] + EntropyGen[iStage*2][config->GetnSpan_iZones(iStage*2)];

    }

    /*---Compute turbo performance for full machine---*/
    FluidModel->SetTDState_Ps(PressureOut[nBladesRow-1][config->GetnSpan_iZones(nBladesRow-1)], EntropyIn[0][config->GetnSpan_iZones(0)]);
    EnthalpyOutIs[nBladesRow + nStages][nSpanWiseSections]          = FluidModel->GetStaticEnergy() + PressureOut[nBladesRow-1][config->GetnSpan_iZones(nBladesRow-1)]/FluidModel->GetDensity();
    FluidModel->SetTDState_Prho(PressureOut[nBladesRow-1][config->GetnSpan_iZones(nBladesRow-1)], DensityOut[nBladesRow-1][config->GetnSpan_iZones(nBladesRow-1)]);
    absVel2 = 0.0;
    for (iDim = 0; iDim<nDim;iDim++) absVel2 += TurboVelocityOut[nBladesRow-1][config->GetnSpan_iZones(nBladesRow-1)][iDim]*TurboVelocityOut[nBladesRow-1][config->GetnSpan_iZones(nBladesRow-1)][iDim];
    TotalEnthalpyOutIs[nBladesRow + nStages][nSpanWiseSections]     = EnthalpyOutIs[nBladesRow + nStages][nSpanWiseSections] + 0.5*absVel2;

    TotalTotalEfficiency[nBladesRow + nStages][nSpanWiseSections]   = (TotalEnthalpyIn[0][config->GetnSpan_iZones(0)] - TotalEnthalpyOut[nBladesRow-1][config->GetnSpan_iZones(nBladesRow-1)]);
    TotalTotalEfficiency[nBladesRow + nStages][nSpanWiseSections]  /= (TotalEnthalpyIn[0][config->GetnSpan_iZones(0)] - TotalEnthalpyOutIs[nBladesRow + nStages][nSpanWiseSections]);
    TotalStaticEfficiency[nBladesRow +nStages][nSpanWiseSections]   = (TotalEnthalpyIn[0][config->GetnSpan_iZones(0)] - TotalEnthalpyOut[nBladesRow-1][config->GetnSpan_iZones(nBladesRow-1)]);
    TotalStaticEfficiency[nBladesRow +nStages][nSpanWiseSections]  /= (TotalEnthalpyIn[0][config->GetnSpan_iZones(0)] - EnthalpyOutIs[nBladesRow + nStages][nSpanWiseSections]);
    PressureRatio[nBladesRow + nStages][nSpanWiseSections]          = PressureRatio[0][config->GetnSpan_iZones(0)]*PressureOut[0][config->GetnSpan_iZones(0)]/PressureOut[nBladesRow-1][config->GetnSpan_iZones(nBladesRow-1)];
    MassFlowIn[nBladesRow + nStages][nSpanWiseSections]             = MassFlowIn[0][config->GetnSpan_iZones(0)];
    MassFlowOut[nBladesRow + nStages][nSpanWiseSections]            = MassFlowOut[nBladesRow-1][config->GetnSpan_iZones(nBladesRow-1)];

    EntropyGen[nBladesRow + nStages][nSpanWiseSections]             = 0.0;
    for(iBlade = 0; iBlade < nBladesRow; iBlade++ ){
      EntropyGen[nBladesRow + nStages][nSpanWiseSections]          += EntropyGen[iBlade][config->GetnSpan_iZones(iBlade)];
    }
  }

  delete [] relVel;

}