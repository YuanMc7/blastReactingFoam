/*---------------------------------------------------------------------------*\
  =========                 |
  \\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox
   \\    /   O peration     |
    \\  /    A nd           | Copyright (C) 2019 Synthetik Applied Technologies
     \\/     M anipulation  |
-------------------------------------------------------------------------------
License
    This file is derivative work of OpenFOAM.

    OpenFOAM is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    OpenFOAM is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License
    along with OpenFOAM.  If not, see <http://www.gnu.org/licenses/>.

\*---------------------------------------------------------------------------*/

#include "reactingCompressibleSystem.H"
#include "fvm.H"
#include "addToRunTimeSelectionTable.H"

// * * * * * * * * * * * * * Static member functions * * * * * * * * * * * * //

namespace Foam
{
    defineTypeNameAndDebug(reactingCompressibleSystem, 0);
}

// * * * * * * * * * * * * * * * * Constructors  * * * * * * * * * * * * * * //

Foam::reactingCompressibleSystem::reactingCompressibleSystem
(
    const fvMesh& mesh
)
:
    integrationSystem("phaseCompressibleSystem", mesh),
    thermo_(rhoReactionThermo::New(mesh)),

Qdot_
(
    IOobject
    (
        "Qdot",
        mesh.time().timeName(),
        mesh,
        IOobject::READ_IF_PRESENT,
        IOobject::AUTO_WRITE
    ),
    mesh,
    dimensionedScalar("Qdot", dimEnergy/dimVolume/dimTime, 0.0)
),

    MachNo_
    (
        IOobject
        (
            "MachNo",
            mesh.time().timeName(),
            mesh,
            IOobject::READ_IF_PRESENT,
            IOobject::AUTO_WRITE
        ),
        mesh,
        dimensionedScalar("MachNo", dimless, 1)
    ),

    rho_
    (
        IOobject
        (
            "rho",
            mesh.time().timeName(),
            mesh,
            IOobject::READ_IF_PRESENT,
            IOobject::AUTO_WRITE
        ),
        mesh,
        dimensionedScalar("rho", dimDensity, 0.0)
    ),
    U_
    (
        IOobject
        (
            "U",
            mesh.time().timeName(),
            mesh,
            IOobject::MUST_READ,
            IOobject::AUTO_WRITE
        ),
        mesh
    ),
    p_(thermo_->p()),
    T_(thermo_->T()),
    e_(thermo_->he()),
    rhoU_
    (
        IOobject
        (
            "rhoU",
            mesh.time().timeName(),
            mesh,
            IOobject::NO_READ,
            IOobject::NO_WRITE
        ),
        rho_*U_
    ),
    rhoE_
    (
        IOobject
        (
            "rhoE",
            mesh.time().timeName(),
            mesh,
            IOobject::NO_READ,
            IOobject::NO_WRITE
        ),
        mesh,
        dimensionedScalar("0", dimDensity*sqr(dimVelocity), 0.0)
    ),
    phi_
    (
        IOobject
        (
            "phi",
            mesh.time().timeName(),
            mesh
        ),
        mesh,
        dimensionedScalar("0", dimVelocity*dimArea, 0.0)
    ),
    rhoPhi_
    (
        IOobject
        (
            "rhoPhi",
            mesh.time().timeName(),
            mesh,
            IOobject::NO_READ,
            IOobject::NO_WRITE
        ),
        mesh,
        dimensionedScalar("0", dimDensity*dimVelocity*dimArea, 0.0)
    ),
    rhoUPhi_
    (
        IOobject
        (
            "rhoUPhi",
            mesh.time().timeName(),
            mesh
        ),
        mesh,
        dimensionedVector("0", dimDensity*sqr(dimVelocity)*dimArea, Zero)
    ),
    rhoEPhi_
    (
        IOobject
        (
            "rhoEPhi",
            mesh.time().timeName(),
            mesh
        ),
        mesh,
        dimensionedScalar("0", dimDensity*pow3(dimVelocity)*dimArea, 0.0)
    ),
    fluxScheme_(fluxScheme::New(mesh)),
    g_(mesh.lookupObject<uniformDimensionedVectorField>("g"))
{
    this->lookupAndInitialize();

    thermo_->validate("compressibleSystem", "e");
    rho_ = thermo_->rho();

    Switch useChemistry
    (
        word(thermo_->subDict("thermoType").lookup("mixture"))
     == "reactingMixture"
    );

   // if (min(thermo_->mu()).value() > small || useChemistry)
   // {
        turbulence_.set
        (
            compressible::turbulenceModel::New
            (
                rho_,
                U_,
                rhoPhi_,
                thermo_()
            ).ptr()
        );
   // }

   // if (useChemistry)
   // {
        reaction_.set
        (
            CombustionModel<rhoReactionThermo>::New
            (
                refCast<rhoReactionThermo>(thermo_()),
                turbulence_()
            ).ptr()
        );
        word inertSpecie(thermo_->lookup("inertSpecie"));
        inertIndex_ = thermo_->composition().species()[inertSpecie];

        YsOld_.setSize(thermo_->composition().species().size());
        deltaRhoYs_.setSize(thermo_->composition().species().size());
        forAll(YsOld_, i)
        {
            YsOld_.set(i, new PtrList<volScalarField>());
            deltaRhoYs_.set(i, new PtrList<volScalarField>());
        }
   // }

    IOobject radIO
    (
        "radiationProperties",
        mesh.time().constant(),
        mesh
    );
    if (radIO.typeHeaderOk<IOdictionary>(true))
    {
        radiation_ = radiationModel::New(T_);
    }
    else
    {
        dictionary radDict;
        radDict.add("radiationModel", "none");
        radiation_ = radiationModel::New(radDict, T_);
    }
    encode();
}


// * * * * * * * * * * * * * * * * Destructor  * * * * * * * * * * * * * * * //

Foam::reactingCompressibleSystem::~reactingCompressibleSystem()
{}

// * * * * * * * * * * * * * * * Member Functions  * * * * * * * * * * * * * //

void Foam::reactingCompressibleSystem::solve()
{
    volScalarField rhoOld(rho_);
    volVectorField rhoUOld(rhoU_);
    volScalarField rhoEOld(rhoE_);

    //- Store old values
    this->storeAndBlendOld(rhoOld, rhoOld_);
    this->storeAndBlendOld(rhoUOld, rhoUOld_);
    this->storeAndBlendOld(rhoEOld, rhoEOld_);

    volScalarField deltaRho(fvc::div(rhoPhi_));
    volVectorField deltaRhoU(fvc::div(rhoUPhi_) - g_*rho_);
    volScalarField deltaRhoE
    (
        fvc::div(rhoEPhi_)
      - (rhoU_ & g_)
    );

    //- Store changed in mass, momentum and energy
    this->storeAndBlendDelta(deltaRho, deltaRho_);
    this->storeAndBlendDelta(deltaRhoU, deltaRhoU_);
    this->storeAndBlendDelta(deltaRhoE, deltaRhoE_);


    dimensionedScalar dT = rho_.time().deltaT();
    rho_ = rhoOld - dT*deltaRho;
    rho_.correctBoundaryConditions();

    vector solutionDs((vector(rho_.mesh().solutionD()) + vector::one)/2.0);
    rhoU_ = cmptMultiply(rhoUOld - dT*deltaRhoU, solutionDs);
    rhoE_ = rhoEOld - dT*deltaRhoE;

    if (reaction_.valid())
    {
        PtrList<volScalarField>& Ys = thermo_->composition().Y();
        volScalarField Yt(0.0*Ys[0]);
        forAll(Ys, i)
        {
            if (i != inertIndex_ && thermo_->composition().active(i))
            {
                volScalarField YOld(Ys[i]);
                this->storeAndBlendOld(YOld, YsOld_[i]);

                volScalarField deltaRhoY
                (
                    fvc::div(fluxScheme_->interpolate(Ys[i], "Yi")*rhoPhi_)
                );
                this->storeAndBlendDelta(deltaRhoY, deltaRhoYs_[i]);

                Ys[i] = (YOld*rhoOld - dT*deltaRhoY)/rho_;
                Ys[i].correctBoundaryConditions();

                Ys[i].max(0.0);
                Yt += Ys[i];
            }
            Ys[inertIndex_] = scalar(1) - Yt;
            Ys[inertIndex_].max(0.0);
        }
    }
}


void Foam::reactingCompressibleSystem::postUpdate()
{
    if (!turbulence_.valid())
    {
        return;
    }

    this->decode();

    volScalarField muEff("muEff", turbulence_->muEff());
    volTensorField tauMC("tauMC", muEff*dev2(Foam::T(fvc::grad(U_))));

    fvVectorMatrix UEqn
    (
        fvm::ddt(rho_, U_) - fvc::ddt(rho_, U_)
      + turbulence_->divDevRhoReff(U_)
    );

    UEqn.solve();

    rhoU_ = rho_*U_;

    fvScalarMatrix eEqn
    (
        fvm::ddt(rho_, e_) - fvc::ddt(rho_, e_)
      - fvm::laplacian(turbulence_->alphaEff(), e_)
    );

    if (reaction_.valid())
    {
        Info<< "Solving reactions" << endl;
        reaction_->correct();

        eEqn -= reaction_->Qdot();

        Qdot_ = reaction_->Qdot();    //YMC

        PtrList<volScalarField>& Y = thermo_->composition().Y();
        volScalarField Yt(0.0*Y[0]);
        forAll(Y, i)
        {
            if (i != inertIndex_ && thermo_->composition().active(i))
            {
                volScalarField& Yi = Y[i];
                fvScalarMatrix YiEqn
                (
                    fvm::ddt(rho_, Yi)
                  - fvc::ddt(rho_, Yi)
                  - fvm::laplacian(turbulence_->muEff(), Yi)
                 ==
                    reaction_->R(Yi)
                );
                YiEqn.solve("Yi");


                Yi.max(0.0);
                Yt += Yi;
            }
        }
        Y[inertIndex_] = scalar(1) - Yt;
        Y[inertIndex_].max(0.0);
    }

    eEqn.solve();
    rhoE_ = rho_*(e_ + 0.5*magSqr(U_)); // Includes change to total energy from viscous term in momentum equation

    thermo_->correct();
    p_.ref() = rho_()/thermo_->psi()();
    p_.correctBoundaryConditions();
    rho_.boundaryFieldRef() ==
        thermo_->psi().boundaryField()*p_.boundaryField();

    turbulence_->correct();
}


void Foam::reactingCompressibleSystem::clearODEFields()
{
    fluxScheme_->clear();
    this->clearOld(rhoOld_);
    this->clearOld(rhoUOld_);
    this->clearOld(rhoEOld_);

    this->clearDelta(deltaRho_);
    this->clearDelta(deltaRhoU_);
    this->clearDelta(deltaRhoE_);

    if (reaction_.valid())
    {
        forAll(YsOld_, i)
        {
            this->clearOld(YsOld_[i]);
            this->clearDelta(deltaRhoYs_[i]);
        }
    }
}


void Foam::reactingCompressibleSystem::update()
{
    decode();
    fluxScheme_->update
    (
        rho_,
        U_,
        e_,
        p_,
        speedOfSound()(),
        phi_,
        rhoPhi_,
        rhoUPhi_,
        rhoEPhi_
    );
}


void Foam::reactingCompressibleSystem::decode()
{
    thermo_->rho() = rho_;

    U_.ref() = rhoU_()/rho_();
    U_.correctBoundaryConditions();

    rhoU_.boundaryFieldRef() = rho_.boundaryField()*U_.boundaryField();

    volScalarField E(rhoE_/rho_);
    e_.ref() = E() - 0.5*magSqr(U_());
    e_.correctBoundaryConditions();

    rhoE_.boundaryFieldRef() =
        rho_.boundaryField()
       *(
            e_.boundaryField()
          + 0.5*magSqr(U_.boundaryField())
        );

    thermo_->correct();
    p_.ref() = rho_/thermo_->psi();
    p_.correctBoundaryConditions();
    rho_.boundaryFieldRef() ==
        thermo_->psi().boundaryField()*p_.boundaryField();
}


void Foam::reactingCompressibleSystem::encode()
{
    rho_ = thermo_->rho();
    rhoU_ = rho_*U_;
    rhoE_ = rho_*(e_ + 0.5*magSqr(U_));
	
	MachNo_ = mag(U_) / speedOfSound();      //YMC
}


Foam::tmp<Foam::volScalarField>
Foam::reactingCompressibleSystem::speedOfSound() const
{
    return sqrt(thermo_->Cp()/(thermo_->Cv()*thermo_->psi()));
}


Foam::tmp<Foam::volScalarField> Foam::reactingCompressibleSystem::Cv() const
{
    return thermo_->Cv();
}


Foam::tmp<Foam::volScalarField> Foam::reactingCompressibleSystem::mu() const
{
    return thermo_->mu();
}


Foam::tmp<Foam::scalarField>
Foam::reactingCompressibleSystem::mu(const label patchi) const
{
    return thermo_->mu(patchi);
}

Foam::tmp<Foam::volScalarField> Foam::reactingCompressibleSystem::nu() const
{
    return thermo_->nu();
}

Foam::tmp<Foam::scalarField>
Foam::reactingCompressibleSystem::nu(const label patchi) const
{
    return thermo_->nu(patchi);
}

Foam::tmp<Foam::volScalarField>
Foam::reactingCompressibleSystem::alpha() const
{
    return thermo_->alpha();
}

Foam::tmp<Foam::scalarField>
Foam::reactingCompressibleSystem::alpha(const label patchi) const
{
    return thermo_->alpha(patchi);
}

Foam::tmp<Foam::volScalarField> Foam::reactingCompressibleSystem::alphaEff
(
    const volScalarField& alphat
) const
{
    return thermo_->alphaEff(alphat);
}

Foam::tmp<Foam::scalarField> Foam::reactingCompressibleSystem::alphaEff
(
    const scalarField& alphat,
    const label patchi
) const
{
    return thermo_->alphaEff(alphat, patchi);
}

Foam::tmp<Foam::volScalarField>
Foam::reactingCompressibleSystem::alphahe() const
{
    return thermo_->alphahe();
}

Foam::tmp<Foam::scalarField>
Foam::reactingCompressibleSystem::alphahe(const label patchi) const
{
    return thermo_->alphahe(patchi);
}

Foam::tmp<Foam::volScalarField> Foam::reactingCompressibleSystem::kappa() const
{
    return thermo_->kappa();
}

Foam::tmp<Foam::scalarField>
Foam::reactingCompressibleSystem::kappa(const label patchi) const
{
    return thermo_->kappa(patchi);
}

Foam::tmp<Foam::volScalarField> Foam::reactingCompressibleSystem::kappaEff
(
    const volScalarField& alphat
) const
{
    return thermo_->kappaEff(alphat);
}

Foam::tmp<Foam::scalarField> Foam::reactingCompressibleSystem::kappaEff
(
    const scalarField& alphat,
    const label patchi
) const
{
    return thermo_->kappaEff(alphat, patchi);
}
// ************************************************************************* //
