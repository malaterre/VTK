/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkClipDataSet.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
#include "vtkClipDataSet.h"

#include "vtkCellArray.h"
#include "vtkCellData.h"
#include "vtkClipVolume.h"
#include "vtkExecutive.h"
#include "vtkFloatArray.h"
#include "vtkGenericCell.h"
#include "vtkImageData.h"
#include "vtkImplicitFunction.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkIntArray.h"
#include "vtkMergePoints.h"
#include "vtkObjectFactory.h"
#include "vtkPointData.h"
#include "vtkUnsignedCharArray.h"
#include "vtkUnstructuredGrid.h"

#include <math.h>

vtkCxxRevisionMacro(vtkClipDataSet, "1.40");
vtkStandardNewMacro(vtkClipDataSet);
vtkCxxSetObjectMacro(vtkClipDataSet,ClipFunction,vtkImplicitFunction);

//----------------------------------------------------------------------------
// Construct with user-specified implicit function; InsideOut turned off; value
// set to 0.0; and generate clip scalars turned off.
vtkClipDataSet::vtkClipDataSet(vtkImplicitFunction *cf)
{
  this->ClipFunction = cf;
  this->InsideOut = 0;
  this->Locator = NULL;
  this->Value = 0.0;
  this->GenerateClipScalars = 0;

  this->GenerateClippedOutput = 0;
  this->MergeTolerance = 0.01;

  this->SetNumberOfOutputPorts(2);
  vtkUnstructuredGrid *output2 = vtkUnstructuredGrid::New();
  this->GetExecutive()->SetOutputData(1, output2);
  output2->Delete();

  this->InputScalarsSelection = NULL;
}

//----------------------------------------------------------------------------
vtkClipDataSet::~vtkClipDataSet()
{
  if ( this->Locator )
    {
    this->Locator->UnRegister(this);
    this->Locator = NULL;
    }
  this->SetClipFunction(NULL);
  this->SetInputScalarsSelection(NULL);
}

//----------------------------------------------------------------------------
// Overload standard modified time function. If Clip functions is modified,
// then this object is modified as well.
unsigned long vtkClipDataSet::GetMTime()
{
  unsigned long mTime=this->Superclass::GetMTime();
  unsigned long time;

  if ( this->ClipFunction != NULL )
    {
    time = this->ClipFunction->GetMTime();
    mTime = ( time > mTime ? time : mTime );
    }
  if ( this->Locator != NULL )
    {
    time = this->Locator->GetMTime();
    mTime = ( time > mTime ? time : mTime );
    }

  return mTime;
}

vtkUnstructuredGrid *vtkClipDataSet::GetClippedOutput()
{
  if (!this->GenerateClippedOutput)
    {
    return NULL;
    }
  return vtkUnstructuredGrid::SafeDownCast(
    this->GetExecutive()->GetOutputData(1));
}

//----------------------------------------------------------------------------
//
// Clip through data generating surface.
//
int vtkClipDataSet::RequestData(
  vtkInformation *vtkNotUsed(request),
  vtkInformationVector **inputVector,
  vtkInformationVector *outputVector)
{
  // get the info objects
  vtkInformation *inInfo = inputVector[0]->GetInformationObject(0);
  vtkInformation *outInfo = outputVector->GetInformationObject(0);

  // get the input and ouptut
  vtkDataSet *input = vtkDataSet::SafeDownCast(
    inInfo->Get(vtkDataObject::DATA_OBJECT()));
  vtkUnstructuredGrid *output = vtkUnstructuredGrid::SafeDownCast(
    outInfo->Get(vtkDataObject::DATA_OBJECT()));

  vtkUnstructuredGrid *clippedOutput = this->GetClippedOutput();
  
  vtkIdType numPts = input->GetNumberOfPoints();
  vtkIdType numCells = input->GetNumberOfCells();
  vtkPointData *inPD=input->GetPointData(), *outPD = output->GetPointData();
  vtkCellData *inCD=input->GetCellData();
  vtkCellData *outCD[2];
  vtkPoints *newPoints;
  vtkFloatArray *cellScalars; 
  vtkDataArray *clipScalars;
  vtkPoints *cellPts;
  vtkIdList *cellIds;
  double s;
  vtkIdType npts;
  vtkIdType *pts;
  int cellType = 0;
  vtkIdType i;
  int j;
  vtkIdType estimatedSize;
  vtkUnsignedCharArray *types[2];
  types[0] = types[1] = 0;
  vtkIdTypeArray *locs[2];
  locs[0] = locs[1] = 0;
  int numOutputs = 1;
  
  vtkDebugMacro(<< "Clipping dataset");
  
  int inputObjectType = input->GetDataObjectType();

  // if we have volumes
  if (inputObjectType == VTK_STRUCTURED_POINTS || 
      inputObjectType == VTK_IMAGE_DATA)
    {
    int dimension;
    int *dims = vtkImageData::SafeDownCast(input)->GetDimensions();
    for (dimension=3, i=0; i<3; i++)
      {
      if ( dims[i] <= 1 )
        {
        dimension--;
        }
      }
    if ( dimension >= 3 )
      {
      this->ClipVolume(input, output);
      return 1;
      }
     }

  // Initialize self; create output objects
  //
  if ( numPts < 1 )
    {
    vtkDebugMacro(<<"No data to clip");
    return 1;
    }

  if ( !this->ClipFunction && this->GenerateClipScalars )
    {
    vtkErrorMacro(<<"Cannot generate clip scalars if no clip function defined");
    return 1;
    }

  // allocate the output and associated helper classes
  estimatedSize = numCells;
  estimatedSize = estimatedSize / 1024 * 1024; //multiple of 1024
  if (estimatedSize < 1024)
    {
    estimatedSize = 1024;
    }
  cellScalars = vtkFloatArray::New();
  cellScalars->Allocate(VTK_CELL_SIZE);
  vtkCellArray *conn[2];
  conn[0] = conn[1] = 0;
  conn[0] = vtkCellArray::New();
  conn[0]->Allocate(estimatedSize,estimatedSize/2);
  conn[0]->InitTraversal();
  types[0] = vtkUnsignedCharArray::New();
  types[0]->Allocate(estimatedSize,estimatedSize/2);
  locs[0] = vtkIdTypeArray::New();
  locs[0]->Allocate(estimatedSize,estimatedSize/2);
  if ( this->GenerateClippedOutput )
    {
    numOutputs = 2;
    conn[1] = vtkCellArray::New();
    conn[1]->Allocate(estimatedSize,estimatedSize/2);
    conn[1]->InitTraversal();
    types[1] = vtkUnsignedCharArray::New();
    types[1]->Allocate(estimatedSize,estimatedSize/2);
    locs[1] = vtkIdTypeArray::New();
    locs[1]->Allocate(estimatedSize,estimatedSize/2);
    }
  newPoints = vtkPoints::New();
  newPoints->Allocate(numPts,numPts/2);
  
  // locator used to merge potentially duplicate points
  if ( this->Locator == NULL )
    {
    this->CreateDefaultLocator();
    }
  this->Locator->InitPointInsertion (newPoints, input->GetBounds());

  // Determine whether we're clipping with input scalars or a clip function
  // and do necessary setup.
  if ( this->ClipFunction )
    {
    vtkFloatArray *tmpScalars = vtkFloatArray::New();
    tmpScalars->SetNumberOfTuples(numPts);
    tmpScalars->SetName("ClipDataSetScalars");
    inPD = vtkPointData::New();
    inPD->ShallowCopy(input->GetPointData());//copies original
    if ( this->GenerateClipScalars )
      {
      inPD->SetScalars(tmpScalars);
      }
    for ( i=0; i < numPts; i++ )
      {
      s = this->ClipFunction->FunctionValue(input->GetPoint(i));
      tmpScalars->SetTuple1(i,s);
      }
    clipScalars = tmpScalars;
    }
  else //using input scalars
    {
    clipScalars = inPD->GetScalars(this->InputScalarsSelection);
    if ( !clipScalars )
      {
      for ( i=0; i<2; i++ )
        {
        if (conn[i])
          {
          conn[i]->Delete();
          }
        if (types[i])
          {
          types[i]->Delete();
          }
        if (locs[i])
          {
          locs[i]->Delete();
          }
        }
      cellScalars->Delete();
      newPoints->Delete();
      vtkErrorMacro(<<"Cannot clip without clip function or input scalars");
      return 1;
      }
    }
    
  if ( !this->GenerateClipScalars && 
       !input->GetPointData()->GetScalars(this->InputScalarsSelection))
    {
    outPD->CopyScalarsOff();
    }
  else
    {
    outPD->CopyScalarsOn();
    }
  outPD->InterpolateAllocate(inPD,estimatedSize,estimatedSize/2);
  outCD[0] = output->GetCellData();
  outCD[0]->CopyAllocate(inCD,estimatedSize,estimatedSize/2);
  if ( this->GenerateClippedOutput )
    {
    outCD[1] = clippedOutput->GetCellData();
    outCD[1]->CopyAllocate(inCD,estimatedSize,estimatedSize/2);
    }

  //Process all cells and clip each in turn
  //
  int abort=0;
  vtkIdType updateTime = numCells/20 + 1;  // update roughly every 5%
  vtkGenericCell *cell = vtkGenericCell::New();
  int num[2]; num[0]=num[1]=0;
  int numNew[2]; numNew[0]=numNew[1]=0;
  for (vtkIdType cellId=0; cellId < numCells && !abort; cellId++)
    {
    if ( !(cellId % updateTime) )
      {
      this->UpdateProgress((double)cellId / numCells);
      abort = this->GetAbortExecute();
      }

    input->GetCell(cellId,cell);
    cellPts = cell->GetPoints();
    cellIds = cell->GetPointIds();
    npts = cellPts->GetNumberOfPoints();

    // evaluate implicit cutting function
    for ( i=0; i < npts; i++ )
      {
      s = clipScalars->GetComponent(cellIds->GetId(i), 0);
      cellScalars->InsertTuple(i, &s);
      }

    // perform the clipping
    cell->Clip(this->Value, cellScalars, this->Locator, conn[0],
               inPD, outPD, inCD, cellId, outCD[0], this->InsideOut);
    numNew[0] = conn[0]->GetNumberOfCells() - num[0];
    num[0] = conn[0]->GetNumberOfCells();
 
    if ( this->GenerateClippedOutput )
      {
      cell->Clip(this->Value, cellScalars, this->Locator, conn[1],
                 inPD, outPD, inCD, cellId, outCD[1], !this->InsideOut);
      numNew[1] = conn[1]->GetNumberOfCells() - num[1];
      num[1] = conn[1]->GetNumberOfCells();
      }

    for (i=0; i<numOutputs; i++) //for both outputs
      {
      for (j=0; j < numNew[i]; j++) 
        {
        locs[i]->InsertNextValue(conn[i]->GetTraversalLocation());
        conn[i]->GetNextCell(npts,pts);
        
        //For each new cell added, got to set the type of the cell
        switch ( cell->GetCellDimension() )
          {
          case 0: //points are generated--------------------------------
            cellType = (npts > 1 ? VTK_POLY_VERTEX : VTK_VERTEX);
            break;

          case 1: //lines are generated---------------------------------
            cellType = (npts > 2 ? VTK_POLY_LINE : VTK_LINE);
            break;

          case 2: //polygons are generated------------------------------
            cellType = (npts == 3 ? VTK_TRIANGLE : 
                        (npts == 4 ? VTK_QUAD : VTK_POLYGON));
            break;

          case 3: //tetrahedra or wedges are generated------------------
            cellType = (npts == 4 ? VTK_TETRA : VTK_WEDGE);
            break;
          } //switch

        types[i]->InsertNextValue(cellType);
        } //for each new cell
      } //for both outputs
    } //for each cell

  cell->Delete();
  cellScalars->Delete();

  if ( this->ClipFunction ) 
    {
    clipScalars->Delete();
    inPD->Delete();
    }
  
  output->SetPoints(newPoints);
  output->SetCells(types[0], locs[0], conn[0]);
  conn[0]->Delete();
  types[0]->Delete();
  locs[0]->Delete();

  if ( this->GenerateClippedOutput )
    {
    clippedOutput->SetPoints(newPoints);
    clippedOutput->SetCells(types[1], locs[1], conn[1]);
    conn[1]->Delete();
    types[1]->Delete();
    locs[1]->Delete();
    }
  
  newPoints->Delete();
  this->Locator->Initialize();//release any extra memory
  output->Squeeze();

  return 1;
}

//----------------------------------------------------------------------------
// Specify a spatial locator for merging points. By default, 
// an instance of vtkMergePoints is used.
void vtkClipDataSet::SetLocator(vtkPointLocator *locator)
{
  if ( this->Locator == locator)
    {
    return;
    }
  
  if ( this->Locator )
    {
    this->Locator->UnRegister(this);
    this->Locator = NULL;
    }

  if ( locator )
    {
    locator->Register(this);
    }

  this->Locator = locator;
  this->Modified();
}

//----------------------------------------------------------------------------
void vtkClipDataSet::CreateDefaultLocator()
{
  if ( this->Locator == NULL )
    {
    this->Locator = vtkMergePoints::New();
    this->Locator->Register(this);
    this->Locator->Delete();
    }
}

//----------------------------------------------------------------------------
void vtkClipDataSet::ClipVolume(vtkDataSet *input, vtkUnstructuredGrid *output)
{
  vtkClipVolume *clipVolume = vtkClipVolume::New();
  
  // We cannot set the input directly.  This messes up the partitioning.
  // output->UpdateNumberOfPieces gets set to 1.
  vtkImageData* tmp = vtkImageData::New();
  tmp->ShallowCopy(vtkImageData::SafeDownCast(input));
  
  clipVolume->SetInput(tmp);
  clipVolume->SetValue(this->Value);
  clipVolume->SetInsideOut(this->InsideOut);
  clipVolume->SetClipFunction(this->ClipFunction);
  clipVolume->SetGenerateClipScalars(this->GenerateClipScalars);
  clipVolume->SetGenerateClippedOutput(this->GenerateClippedOutput);
  clipVolume->SetMergeTolerance(this->MergeTolerance);
  clipVolume->SetDebug(this->Debug);
  clipVolume->Update();
  vtkUnstructuredGrid *clipOutput = clipVolume->GetOutput();

  output->CopyStructure(clipOutput);
  output->GetPointData()->ShallowCopy(clipOutput->GetPointData());
  output->GetCellData()->ShallowCopy(clipOutput->GetCellData());
  clipVolume->Delete();
  tmp->Delete();
}

//----------------------------------------------------------------------------
int vtkClipDataSet::FillInputPortInformation(int, vtkInformation *info)
{
  info->Set(vtkAlgorithm::INPUT_REQUIRED_DATA_TYPE(), "vtkDataSet");
  return 1;
}

//----------------------------------------------------------------------------
void vtkClipDataSet::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os,indent);

  os << indent << "Merge Tolerance: " << this->MergeTolerance << "\n";
  if ( this->ClipFunction )
    {
    os << indent << "Clip Function: " << this->ClipFunction << "\n";
    }
  else
    {
    os << indent << "Clip Function: (none)\n";
    }
  os << indent << "InsideOut: " << (this->InsideOut ? "On\n" : "Off\n");
  os << indent << "Value: " << this->Value << "\n";
  if ( this->Locator )
    {
    os << indent << "Locator: " << this->Locator << "\n";
    }
  else
    {
    os << indent << "Locator: (none)\n";
    }

  os << indent << "Generate Clip Scalars: " 
     << (this->GenerateClipScalars ? "On\n" : "Off\n");

  os << indent << "Generate Clipped Output: " 
     << (this->GenerateClippedOutput ? "On\n" : "Off\n");

  if (this->InputScalarsSelection)
    {
    os << indent << "InputScalarsSelection: " 
       << this->InputScalarsSelection << endl;
    }
}
