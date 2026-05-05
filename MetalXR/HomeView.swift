//
//  HomeView.swift
//  MetalXR
//
//  Created by Eilionoir Tunnicliff on 5/3/23.
//

import Foundation
import SwiftUI

struct HomeView: View {
    var body: some View {
        VStack (
            alignment: .leading,
            spacing: 12
        ) {
            Text("MetalXR")
                .font(.title)
                .fontWeight(.bold)
                .multilineTextAlignment(.leading)

            Text("Quest client installation is complete. Use the scripts in the repository to launch Unity with an OpenXR runtime on macOS.")
                .foregroundStyle(.secondary)

            NavigationLink { InitialView() } label: { Text("Back") }
                
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding()
    }
}
